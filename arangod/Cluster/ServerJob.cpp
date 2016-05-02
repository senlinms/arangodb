////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "ServerJob.h"

#include "Basics/MutexLocker.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/HeartbeatThread.h"
#include "Dispatcher/DispatcherQueue.h"
#include "Logger/Logger.h"
#include "RestServer/DatabaseFeature.h"
#include "V8/v8-utils.h"
#include "V8Server/V8Context.h"
#include "V8Server/V8DealerFeature.h"
#include "VocBase/server.h"
#include "VocBase/vocbase.h"

#include <iostream>

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::rest;

static arangodb::Mutex ExecutorLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a new db server job
////////////////////////////////////////////////////////////////////////////////

ServerJob::ServerJob(HeartbeatThread* heartbeat)
    : Job("HttpServerJob"),
      _heartbeat(heartbeat),
      _shutdown(0),
      _abandon(false) {}

////////////////////////////////////////////////////////////////////////////////
/// @brief destructs a db server job
////////////////////////////////////////////////////////////////////////////////

ServerJob::~ServerJob() {}

void ServerJob::work() {
  LOG(TRACE) << "starting plan update handler";

  if (_shutdown != 0) {
    return;
  }

  _heartbeat->setReady();

  ServerJobResult result;
  {
    // only one plan change at a time
    MUTEX_LOCKER(mutexLocker, ExecutorLock);

    result = execute();
  }

  _heartbeat->removeDispatchedJob(result);
}

bool ServerJob::cancel() { return false; }

void ServerJob::cleanup(DispatcherQueue* queue) {
  queue->removeJob(this);
  delete this;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief execute job
////////////////////////////////////////////////////////////////////////////////

ServerJobResult ServerJob::execute() {
  // default to system database

  DatabaseFeature* database = 
    ApplicationServer::getFeature<DatabaseFeature>("Database");

  TRI_vocbase_t* const vocbase = database->vocbase();

  ServerJobResult result;
  if (vocbase == nullptr) {
    return result;
  }

  TRI_UseVocBase(vocbase);
  TRI_DEFER(TRI_ReleaseVocBase(vocbase));

  V8Context* context = V8DealerFeature::DEALER->enterContext(vocbase, true);

  if (context == nullptr) {
    return result;
  }

  auto isolate = context->_isolate;

  try {
    v8::HandleScope scope(isolate);

    // execute script inside the context
    auto file = TRI_V8_ASCII_STRING("handle-plan-change");
    auto content =
        TRI_V8_ASCII_STRING("require('@arangodb/cluster').handlePlanChange();");
    
    v8::TryCatch tryCatch;
    v8::Handle<v8::Value> res = TRI_ExecuteJavaScriptString(
        isolate, isolate->GetCurrentContext(), content, file, false);
    
    if (tryCatch.HasCaught()) {
      if (tryCatch.CanContinue()) {
        TRI_LogV8Exception(isolate, &tryCatch);
        return result;
      }
    }

    if (res->IsObject()) {
      v8::Handle<v8::Object> o = res->ToObject();

      v8::Handle<v8::Array> names = o->GetOwnPropertyNames();
      uint32_t const n = names->Length();

      
      for (uint32_t i = 0; i < n; ++i) {
        v8::Handle<v8::Value> key = names->Get(i);
        v8::String::Utf8Value str(key);

        v8::Handle<v8::Value> value = o->Get(key);
        
        if (value->IsNumber()) {
          if (strcmp(*str, "plan") == 0) {
            result.planVersion = static_cast<uint64_t>(value->ToUint32()->Value());
          } else if (strcmp(*str, "current") == 0) {
            result.currentVersion = static_cast<uint64_t>(value->ToUint32()->Value());
          }
        }
      }
    }
    result.success = true;
    // invalidate our local cache, even if an error occurred
    ClusterInfo::instance()->flush();
  } catch (...) {
  }

  V8DealerFeature::DEALER->exitContext(context);

  return result;
}
