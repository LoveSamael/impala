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

#ifndef IMPALA_UTIL_DEFAULT_PATH_HANDLERS_H
#define IMPALA_UTIL_DEFAULT_PATH_HANDLERS_H

#include <stdio.h>

namespace impala {

class MemTracker;
class Webserver;

// Adds a set of default path handlers to the webserver to display
// logs and configuration flags
void AddDefaultPathHandlers(Webserver* webserver, MemTracker* process_mem_tracker = NULL);
}

#endif // IMPALA_UTIL_DEFAULT_PATH_HANDLERS_H
