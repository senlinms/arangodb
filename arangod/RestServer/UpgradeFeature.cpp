////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "UpgradeFeature.h"

#include "Cluster/ClusterFeature.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Logger/Logger.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "Replication/ReplicationFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/InitDatabaseFeature.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8Server/V8Context.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/v8-vocbase.h"
#include "VocBase/vocbase.h"
#include "VocBase/Methods/Upgrade.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

UpgradeFeature::UpgradeFeature(
    ApplicationServer* server, int* result,
    std::vector<std::string> const& nonServerFeatures)
    : ApplicationFeature(server, "Upgrade"),
      _upgrade(false),
      _upgradeCheck(true),
      _result(result),
      _nonServerFeatures(nonServerFeatures) {
  setOptional(false);
  requiresElevatedPrivileges(false);
  startsAfter("CheckVersion");
  startsAfter("Cluster");
  startsAfter("Database");
  startsAfter("V8Dealer");
  startsAfter("Aql");
}

void UpgradeFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("database", "Configure the database");

  options->addOldOption("upgrade", "database.auto-upgrade");

  options->addOption("--database.auto-upgrade",
                     "perform a database upgrade if necessary",
                     new BooleanParameter(&_upgrade));

  options->addHiddenOption("--database.upgrade-check",
                           "skip a database upgrade",
                           new BooleanParameter(&_upgradeCheck));
}

void UpgradeFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  if (_upgrade && !_upgradeCheck) {
    LOG_TOPIC(FATAL, arangodb::Logger::FIXME) << "cannot specify both '--database.auto-upgrade true' and "
                  "'--database.upgrade-check false'";
    FATAL_ERROR_EXIT();
  }

  if (!_upgrade) {
    LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "executing upgrade check: not disabling server features";
    return;
  }

  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "executing upgrade procedure: disabling server features";

  ApplicationServer::forceDisableFeatures(_nonServerFeatures);
  
  ReplicationFeature* replicationFeature =
      ApplicationServer::getFeature<ReplicationFeature>("Replication");
  replicationFeature->disableReplicationApplier();

  DatabaseFeature* database =
      ApplicationServer::getFeature<DatabaseFeature>("Database");
  database->enableUpgrade();

  ClusterFeature* cluster =
      ApplicationServer::getFeature<ClusterFeature>("Cluster");
  cluster->forceDisable();
}

void UpgradeFeature::prepare() {
  // need to register tasks before creating any database
  methods::Upgrade::registerTasks();
}

void UpgradeFeature::start() {
  auto init =
      ApplicationServer::getFeature<InitDatabaseFeature>("InitDatabase");
  AuthInfo *ai = AuthenticationFeature::INSTANCE->authInfo();

  // upgrade the database
  if (_upgradeCheck) {
    upgradeDatabase();

    if (!init->restoreAdmin() && !init->defaultPassword().empty() &&
        ServerState::instance()->isSingleServerOrCoordinator()) {
      ai->updateUser("root", [&](AuthUserEntry& entry) {
        entry.updatePassword(init->defaultPassword());
      });
    }
  }

  // change admin user
  if (init->restoreAdmin() &&
      ServerState::instance()->isSingleServerOrCoordinator()) {
    
    Result res = ai->removeAllUsers();
    if (res.fail()) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "failed to clear users: "
                                              << res.errorMessage();
      *_result = EXIT_FAILURE;
      return;
    }

    res = ai->storeUser(true, "root", init->defaultPassword(), true);
    if (res.fail() && res.errorNumber() == TRI_ERROR_USER_NOT_FOUND) {
      res = ai->storeUser(false, "root", init->defaultPassword(), true);
    }

    if (res.fail()) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "failed to create root user: "
                                              << res.errorMessage();
      *_result = EXIT_FAILURE;
      return;
    }
    auto oldLevel = arangodb::Logger::FIXME.level();
    arangodb::Logger::FIXME.setLogLevel(arangodb::LogLevel::INFO);
    LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "Password changed.";
    arangodb::Logger::FIXME.setLogLevel(oldLevel);
    *_result = EXIT_SUCCESS;
  }

  // and force shutdown
  if (_upgrade || init->isInitDatabase() || init->restoreAdmin()) {
    if (init->isInitDatabase()) {
      *_result = EXIT_SUCCESS;
    }

    server()->beginShutdown();
  }
}

void UpgradeFeature::upgradeDatabase() {
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "starting database init/upgrade";

  DatabaseFeature* databaseFeature = application_features::ApplicationServer::getFeature<DatabaseFeature>("Database");
  
  for (auto& name : databaseFeature->getDatabaseNames()) {
    TRI_vocbase_t* vocbase = databaseFeature->lookupDatabase(name);
    TRI_ASSERT(vocbase != nullptr);
    
    methods::UpgradeResult res = methods::Upgrade::startup(vocbase, _upgrade);
    if (res.fail()) {
      char const* typeName = "initialization";
      switch (res.type) {
        case methods::VersionResult::VERSION_MATCH:
        case methods::VersionResult::DOWNGRADE_NEEDED:
        case methods::VersionResult::NO_VERSION_FILE:
          // initialization
          break;
        case methods::VersionResult::UPGRADE_NEEDED:
          typeName = "upgrade";
          if (!_upgrade) {
            LOG_TOPIC(FATAL, arangodb::Logger::FIXME)
            << "Database '" << vocbase->name()
            << "' needs upgrade. Please start the server with the "
            "--database.auto-upgrade option";
          }
          break;
        default:
          break;
      }
      LOG_TOPIC(FATAL, arangodb::Logger::FIXME) << "Database '" << vocbase->name()
      << "' " << typeName << " failed (" << res.errorMessage() << "). "
      "Please inspect the logs from the " << typeName << " procedure";
      FATAL_ERROR_EXIT();
    }
  }

  if (_upgrade) {
    *_result = EXIT_SUCCESS;
    LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "database upgrade passed";
  }

  // and return from the context
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "finished database init/upgrade";
}
