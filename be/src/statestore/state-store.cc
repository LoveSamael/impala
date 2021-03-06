// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <boost/foreach.hpp>

#include "common/status.h"
#include "statestore/state-store.h"
#include "gen-cpp/StateStoreService_types.h"
#include "statestore/failure-detector.h"
#include "util/stopwatch.h"
#include "rpc/thrift-util.h"
#include "util/webserver.h"

#include <boost/thread.hpp>

using namespace impala;
using namespace std;
using namespace boost;

DEFINE_int32(statestore_max_missed_heartbeats, 5, "Maximum number of consecutive "
             "heartbeats an impalad can miss before being declared failed by the "
             "state-store.");
DEFINE_int32(statestore_suspect_heartbeats, 2, "(Advanced) Number of consecutive "
             "heartbeats an impalad can miss before being suspected of failure by the"
             " state-store");
DEFINE_int32(statestore_num_heartbeat_threads, 10, "(Advanced) Number of threads used to "
             " send heartbeats in parallel to all registered subscribers.");

DEFINE_int32(state_store_port, 24000, "port where StateStoreService is running");

// Metric keys
// TODO: Replace 'backend' with 'subscriber' when we can coordinate a change with CM
const string STATESTORE_LIVE_SUBSCRIBERS = "statestore.live-backends";
const string STATESTORE_LIVE_SUBSCRIBERS_LIST = "statestore.live-backends.list";

const StateStore::TopicEntry::Value StateStore::TopicEntry::NULL_VALUE = "";

// Initial version for each Topic registered by a Subscriber. Generally, the Topic will
// have a Version that is the MAX() of all entries in the Topic, but this initial
// value needs to be less than TopicEntry::TOPIC_ENTRY_INITIAL_VERSION to distinguish
// between the case where a Topic is empty and the case where the Topic only contains
// an item with the initial version.
const StateStore::TopicEntry::Version StateStore::Subscriber::TOPIC_INITIAL_VERSION = 0L;

// Used to control the maximum size of the heartbeat input queue, in which there is at
// most one entry per subscriber.
const int32_t STATESTORE_MAX_SUBSCRIBERS = 10000;

// The minimum frequency with which to attempt heartbeats to each subscriber. Subscribers
// may experience larger delays between heartbeats, but will not receive them more
// frequently.
const int32_t MIN_HEARTBEAT_DELTA_MS = 500;

typedef ClientConnection<StateStoreSubscriberClient> StateStoreSubscriberConnection;

class StateStoreThriftIf : public StateStoreServiceIf {
 public:
  StateStoreThriftIf(StateStore* state_store)
      : state_store_(state_store) {
    DCHECK(state_store_ != NULL);
  }

  virtual void RegisterSubscriber(TRegisterSubscriberResponse& response,
                                  const TRegisterSubscriberRequest& params) {
    Status status = state_store_->RegisterSubscriber(params.subscriber_id,
        params.subscriber_location, params.topic_registrations);
    status.ToThrift(&response.status);
  }
 private:
  StateStore* state_store_;
};

void StateStore::TopicEntry::SetValue(const StateStore::TopicEntry::Value& bytes,
    TopicEntry::Version version) {
  DCHECK(bytes == StateStore::TopicEntry::NULL_VALUE || bytes.size() > 0);
  value_ = bytes;
  version_ = version;
}

StateStore::TopicEntry::Version StateStore::Topic::Put(const string& key,
    const StateStore::TopicEntry::Value& bytes) {
  TopicEntryMap::iterator entry_it = entries_.find(key);
  if (entry_it == entries_.end()) {
    entry_it = entries_.insert(make_pair(key, TopicEntry())).first;
  } else {
    // Delete the old item from the version history. There is no need to search the
    // version_history because there should only be at most a single item in the history
    // at any given time
    topic_update_log_.erase(entry_it->second.version());
  }
  entry_it->second.SetValue(bytes, ++last_version_);
  topic_update_log_.insert(make_pair(entry_it->second.version(), key));
  return entry_it->second.version();
}

void StateStore::Topic::DeleteIfVersionsMatch(TopicEntry::Version version,
    const StateStore::TopicEntryKey& key) {
  TopicEntryMap::iterator entry_it = entries_.find(key);
  if (entry_it != entries_.end() && entry_it->second.version() == version) {
    // Add a new entry with the the version history for this deletion and remove
    // the old entry
    topic_update_log_.erase(version);
    topic_update_log_.insert(make_pair(++last_version_, key));
    entry_it->second.SetValue(StateStore::TopicEntry::NULL_VALUE, last_version_);
  }
}

StateStore::Subscriber::Subscriber(const SubscriberId& subscriber_id,
    const TNetworkAddress& network_address,
    const vector<TTopicRegistration>& subscribed_topics)
      : subscriber_id_(subscriber_id), network_address_(network_address) {
  BOOST_FOREACH(const TTopicRegistration& topic, subscribed_topics) {
    TopicState topic_state;
    topic_state.is_transient = topic.is_transient;
    topic_state.last_version = TOPIC_INITIAL_VERSION;
    subscribed_topics_[topic.topic_name] = topic_state;
  }
}

void StateStore::Subscriber::AddTransientUpdate(const TopicId& topic_id,
    const TopicEntryKey& topic_key, TopicEntry::Version version) {
  // Only record the update if the topic is transient
  const Topics::const_iterator topic_it = subscribed_topics_.find(topic_id);
  DCHECK(topic_it != subscribed_topics_.end());
  if (topic_it->second.is_transient == true) {
    transient_entries_[make_pair(topic_id, topic_key)] = version;
  }
}

const StateStore::TopicEntry::Version StateStore::Subscriber::GetMaxVersionForTopic(
    const TopicId& topic_id) const {
  Topics::const_iterator itr = subscribed_topics_.find(topic_id);
  return itr == subscribed_topics_.end() ?
      TOPIC_INITIAL_VERSION : itr->second.last_version;
}

void StateStore::Subscriber::SetMaxVersionForTopic(const TopicId& topic_id,
    TopicEntry::Version version) {
  subscribed_topics_[topic_id].last_version = version;
}

StateStore::StateStore(Metrics* metrics)
  : exit_flag_(false),
    subscriber_heartbeat_threadpool_("statestore",
        "subscriber-heartbeat-worker",
        FLAGS_statestore_num_heartbeat_threads,
        STATESTORE_MAX_SUBSCRIBERS,
        bind<void>(mem_fn(&StateStore::UpdateSubscriber), this, _1, _2)),
    client_cache_(new ClientCache<StateStoreSubscriberClient>()),
    thrift_iface_(new StateStoreThriftIf(this)),
    failure_detector_(
        new MissedHeartbeatFailureDetector(FLAGS_statestore_max_missed_heartbeats,
                                           FLAGS_statestore_suspect_heartbeats)) {

  DCHECK(metrics != NULL);
  num_subscribers_metric_ =
      metrics->CreateAndRegisterPrimitiveMetric(STATESTORE_LIVE_SUBSCRIBERS, 0L);
  subscriber_set_metric_ =
      metrics->RegisterMetric(new SetMetric<string>(STATESTORE_LIVE_SUBSCRIBERS_LIST,
                                                    set<string>()));
  client_cache_->InitMetrics(metrics, "subscriber");
}

void StateStore::RegisterWebpages(Webserver* webserver) {
  Webserver::PathHandlerCallback topics_callback =
      bind<void>(mem_fn(&StateStore::TopicsHandler), this, _1, _2);
  webserver->RegisterPathHandler("/topics", topics_callback);

  Webserver::PathHandlerCallback subscribers_callback =
      bind<void>(mem_fn(&StateStore::SubscribersHandler), this, _1, _2);
  webserver->RegisterPathHandler("/subscribers", subscribers_callback);
}

void StateStore::TopicsHandler(const Webserver::ArgumentMap& args,
                                stringstream* output) {
  (*output) << "<h2>Topics</h2>";
  (*output) << "<table class='table table-striped'>"
            << "<tr><th>Topic Id</th><th>Number of entries</th></tr>";

  lock_guard<mutex> l(topic_lock_);
  BOOST_FOREACH(const TopicMap::value_type& topic, topics_) {
    (*output) << "<tr><td>" << topic.second.id() << "</td>";
    (*output) << "<td>" << topic.second.entries().size() << "</td></tr>";
  }
  (*output) << "</table>";
}

void StateStore::SubscribersHandler(const Webserver::ArgumentMap& args,
                                     stringstream* output) {
  (*output) << "<h2>Subscribers</h2>";
  (*output) << "<table class ='table table-striped'>"
            << "<tr><th>Id</th><th>Address</th><th>Subscribed topics</th>"
            << "<th>Transient entries</th></tr>";
  lock_guard<mutex> l(subscribers_lock_);
  BOOST_FOREACH(const SubscriberMap::value_type& subscriber, subscribers_) {
    (*output) << "<tr><td>" << subscriber.second.id() << "</td><td>"
              << subscriber.second.network_address() << "</td><td>"
              << subscriber.second.subscribed_topics().size() << "</td><td>"
              << subscriber.second.transient_entries().size() << "</td></tr>";
  }
  (*output) << "</table>";
}

Status StateStore::RegisterSubscriber(const SubscriberId& subscriber_id,
    const TNetworkAddress& location,
    const vector<TTopicRegistration>& topic_registrations) {
  if (subscriber_id.empty()) return Status("Subscriber ID cannot be empty string");

  // Create any new topics first, so that when the subscriber is first
  // sent a heartbeat by the worker threads its topics are guaranteed
  // to exist.
  {
    lock_guard<mutex> l(topic_lock_);
    BOOST_FOREACH(const TTopicRegistration& topic, topic_registrations) {
      TopicMap::iterator topic_it = topics_.find(topic.topic_name);
      if (topic_it == topics_.end()) {
        LOG(INFO) << "Creating new topic: ''" << topic.topic_name
                  << "' on behalf of subscriber: '" << subscriber_id;
        topics_.insert(make_pair(topic.topic_name, Topic(topic.topic_name)));
      }
    }
  }
  LOG(INFO) << "Registering: " << subscriber_id;
  {
    lock_guard<mutex> l(subscribers_lock_);
    SubscriberMap::iterator subscriber_it = subscribers_.find(subscriber_id);
    if (subscriber_it != subscribers_.end()) {
      // To avoid having to deal with ownership of a Subscriber that would be replaced by
      // a new registration from the same client while the update loop is processing it,
      // we disallow duplicate registrations. What this usually means is that the client
      // has failed and recovered, but the statestore has not yet detected the
      // failure. Since the statestore is probably in the middle of trying to contact the
      // old subscriber, it won't try and contact the new one until it detects the
      // failure, so it's ok to ignore the request. The subscriber will retry until
      // successful anyhow.
      stringstream ss;
      ss << "Duplicate registration of subscriber: " << subscriber_id;
      LOG(WARNING) << "Duplicate registration of subscriber: " << subscriber_id
                   << ", possible duplicate subscriber IDs or recovering subscriber";
      return Status(ss.str());
    }

    subscribers_.insert(make_pair(subscriber_id,
        Subscriber(subscriber_id, location, topic_registrations)));
    failure_detector_->UpdateHeartbeat(subscriber_id, true);
    num_subscribers_metric_->Update(subscribers_.size());
    subscriber_set_metric_->Add(subscriber_id);
  }

  if (subscriber_heartbeat_threadpool_.GetQueueSize() >= STATESTORE_MAX_SUBSCRIBERS) {
    stringstream ss;
    ss << "Maximum subscriber limit reached: " << STATESTORE_MAX_SUBSCRIBERS;
    lock_guard<mutex> l(subscribers_lock_);
    SubscriberMap::iterator subscriber_it = subscribers_.find(subscriber_id);
    DCHECK(subscriber_it != subscribers_.end());
    subscribers_.erase(subscriber_it);
    LOG(ERROR) << ss.str();
    return Status(ss.str());
  }

  // Add the subscriber to the update queue, with an immediate schedule.
  subscriber_heartbeat_threadpool_.Offer(make_pair(0L, subscriber_id));
  return Status::OK;
}

Status StateStore::ProcessOneSubscriber(Subscriber* subscriber) {
  // First thing: make a list of updates to send
  TUpdateStateRequest update_state_request;
  {
    lock_guard<mutex> l(topic_lock_);
    GatherTopicUpdates(*subscriber, &update_state_request);
  }

  // Second: try and send it
  Status status;
  StateStoreSubscriberConnection client(client_cache_.get(),
                                        subscriber->network_address(), &status);
  RETURN_IF_ERROR(status);

  TUpdateStateResponse response;

  // TODO: Rework the client-cache API so that this dance isn't necessary.
  try {
    client->UpdateState(response, update_state_request);
  } catch (apache::thrift::transport::TTransportException& e) {
    // Client may have been closed due to a failure
    RETURN_IF_ERROR(client.Reopen());
    try {
      client->UpdateState(response, update_state_request);
    } catch (apache::thrift::transport::TTransportException& e) {
      return Status(e.what());
    }
  }
  RETURN_IF_ERROR(Status(response.status));

  // At this point the updates are assumed to have been successfully processed
  // by the subscriber. Update the subscriber's max version of each topic.
  map<TopicEntryKey, TTopicDelta>::const_iterator topic_delta =
      update_state_request.topic_deltas.begin();
  for (; topic_delta != update_state_request.topic_deltas.end(); ++topic_delta) {
    subscriber->SetMaxVersionForTopic(topic_delta->first, topic_delta->second.to_version);
  }

  // Thirdly: perform any / all updates returned by the subscriber
  {
    lock_guard<mutex> l(topic_lock_);
    BOOST_FOREACH(const TTopicDelta& update, response.topic_updates) {
      TopicMap::iterator topic_it = topics_.find(update.topic_name);
      if (topic_it == topics_.end()) {
        VLOG(1) << "Received update for unexpected topic:" << update.topic_name;
        continue;
      }

      // The subscriber sent back their from_version which indicates that they
      // want to reset their max version for this topic to this value. The next update
      // sent will be from this version.
      if (update.__isset.from_version) {
        LOG(INFO) << "Received request for different delta base of topic: "
                  << update.topic_name << " from: " << subscriber->id()
                  << " subscriber from_version: " << update.from_version;
        subscriber->SetMaxVersionForTopic(topic_it->first, update.from_version);
      }

      Topic* topic = &topic_it->second;
      BOOST_FOREACH(const TTopicItem& item, update.topic_entries) {
        TopicEntry::Version version = topic->Put(item.key, item.value);
        subscriber->AddTransientUpdate(update.topic_name, item.key, version);
      }

      BOOST_FOREACH(const string& key, update.topic_deletions) {
        TopicEntry::Version version =
            topic->Put(key, StateStore::TopicEntry::NULL_VALUE);
        subscriber->AddTransientUpdate(update.topic_name, key, version);
      }
    }
  }
  return Status::OK;
}

void StateStore::GatherTopicUpdates(const Subscriber& subscriber,
    TUpdateStateRequest* update_state_request) {
  BOOST_FOREACH(const Subscriber::Topics::value_type& subscribed_topic,
      subscriber.subscribed_topics()) {
    TopicMap::const_iterator topic_it = topics_.find(subscribed_topic.first);
    DCHECK(topic_it != topics_.end());

    TTopicDelta& topic_delta = update_state_request->topic_deltas[subscribed_topic.first];
    topic_delta.topic_name = subscribed_topic.first;

    // If the subscriber version is > 0, send this update as a delta. Otherwise, this
    // is a new subscriber so send them a non-delta update that includes all items
    // in the topic.
    TopicEntry::Version max_version = subscriber.GetMaxVersionForTopic(topic_it->first);
    topic_delta.is_delta = max_version > Subscriber::TOPIC_INITIAL_VERSION;
    topic_delta.__set_from_version(max_version);

    const Topic& topic = topic_it->second;
    TopicUpdateLog::const_iterator next_update =
        topic.topic_update_log().upper_bound(max_version);

    for (; next_update != topic.topic_update_log().end(); ++next_update) {
      TopicEntryMap::const_iterator itr = topic.entries().find(next_update->second);
      DCHECK(itr != topic.entries().end());
      const TopicEntry& topic_entry = itr->second;
      if (topic_entry.value() == StateStore::TopicEntry::NULL_VALUE) {
        topic_delta.topic_deletions.push_back(itr->first);
      } else {
        topic_delta.topic_entries.push_back(TTopicItem());
        TTopicItem& topic_item = topic_delta.topic_entries.back();
        topic_item.key = itr->first;
        // TODO: Does this do a needless copy?
        topic_item.value = topic_entry.value();
      }
    }

    if (topic.topic_update_log().size() > 0) {
      // The largest version for this topic will be the last item in the version
      // history map.
      topic_delta.__set_to_version(topic.topic_update_log().rbegin()->first);
    } else {
      // There are no updates in the version history
      topic_delta.__set_to_version(Subscriber::TOPIC_INITIAL_VERSION);
    }
  }
}

bool StateStore::ShouldExit() {
  lock_guard<mutex> l(exit_flag_lock_);
  return exit_flag_;
}

void StateStore::SetExitFlag() {
  lock_guard<mutex> l(exit_flag_lock_);
  exit_flag_ = true;
  subscriber_heartbeat_threadpool_.Shutdown();
}

void StateStore::UpdateSubscriber(int thread_id,
    const ScheduledSubscriberUpdate& update) {
  // Wait until update time
  int64_t now = TimestampValue::local_time_micros();
  int64_t diff = update.first - now;
  while (diff > 0) {
    usleep(diff * 1000L);
    now = TimestampValue::local_time_micros();
    diff = now - update.first;
  }

  Subscriber* subscriber = NULL;
  {
    lock_guard<mutex> l(subscribers_lock_);
    SubscriberMap::iterator it = subscribers_.find(update.second);
    if (it == subscribers_.end()) return;
    subscriber = &(it->second);
  }

  // Give up the lock here so that others can get to the queue
  Status status = ProcessOneSubscriber(subscriber);
  if (!status.ok()) {
    if (failure_detector_->GetPeerState(subscriber->id()) == FailureDetector::OK) {
      LOG(INFO) << "Unable to update subscriber at " << subscriber->network_address()
                << ",  received error " << status.GetErrorMsg();
    }
  }

  if (failure_detector_->UpdateHeartbeat(subscriber->id(), status.ok()) ==
      FailureDetector::FAILED) {
    // TODO: Consider if a metric to track the number of failures would be useful.
    lock_guard<mutex> l(subscribers_lock_);
    // TODO: Make clear that 'failure' isn't necessarily an error
    LOG(INFO) << "Subscriber: " << subscriber->id()
              << " has either failed or disconnected.";
    // Close all active clients so that the next attempt to use them causes a Reopen
    client_cache_->CloseConnections(subscriber->network_address());

    // Delete all transient entries
    lock_guard<mutex> topic_lock(topic_lock_);
    BOOST_FOREACH(StateStore::Subscriber::TransientEntryMap::value_type entry,
                  subscriber->transient_entries()) {
      StateStore::TopicMap::iterator topic_it = topics_.find(entry.first.first);
      DCHECK(topic_it != topics_.end());
      topic_it->second.DeleteIfVersionsMatch(entry.second, // version
                                             entry.first.second); // key
    }
    num_subscribers_metric_->Increment(-1L);
    subscriber_set_metric_->Remove(subscriber->id());
    subscribers_.erase(subscriber->id());
  } else {
    // TODO: Consider changing heartbeat frequency dynamically; if e.g. last heartbeat was
    // a failure may want to send another heartbeat more quickly to confirm.
    int64_t now = TimestampValue::local_time_micros();
    subscriber_heartbeat_threadpool_.Offer(
        make_pair(now + MIN_HEARTBEAT_DELTA_MS, subscriber->id()));
  }
}

Status StateStore::MainLoop() {
  subscriber_heartbeat_threadpool_.Join();
  return Status::OK;
}
