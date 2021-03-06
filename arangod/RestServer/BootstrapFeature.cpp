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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "RestServer/BootstrapFeature.h"

#include "Agency/AgencyComm.h"
#include "Aql/QueryList.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "Logger/Logger.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "Rest/GeneralResponse.h"
#include "Rest/Version.h"
#include "RestServer/DatabaseFeature.h"
#include "V8Server/V8DealerFeature.h"

using namespace arangodb;
using namespace arangodb::options;

static std::string const boostrapKey = "Bootstrap";

BootstrapFeature::BootstrapFeature(
    application_features::ApplicationServer* server)
    : ApplicationFeature(server, "Bootstrap"), _isReady(false), _bark(false) {
  startsAfter("Endpoint");
  startsAfter("Scheduler");
  startsAfter("Server");
  startsAfter("Database");
  startsAfter("Upgrade");
  startsAfter("CheckVersion");
  startsAfter("FoxxQueues");
  startsAfter("GeneralServer");
  startsAfter("Cluster");
  startsAfter("V8Dealer");
}

void BootstrapFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addHiddenOption("hund", "make ArangoDB bark on startup",
                           new BooleanParameter(&_bark));
}

/// must only return if we are boostrap lead or bootstrap is done
static void raceForClusterBootstrap() {
  AgencyComm agency;
  auto ci = ClusterInfo::instance();
  while (true) {
    AgencyCommResult result = agency.getValues(boostrapKey);
    if (!result.successful()) {
      // Error in communication, note that value not found is not an error
      LOG_TOPIC(TRACE, Logger::STARTUP)
      << "raceForClusterBootstrap: no agency communication";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    
    VPackSlice value = result.slice()[0].get(
                std::vector<std::string>({AgencyCommManager::path(), boostrapKey}));
    if (value.isString()) {
      // key was found and is a string
      std::string boostrapVal = value.copyString();
      if (boostrapVal.find("done") != std::string::npos) {
        // all done, let's get out of here:
        LOG_TOPIC(TRACE, Logger::STARTUP)
          << "raceForClusterBootstrap: bootstrap already done";
        return;
      } else if (boostrapVal == ServerState::instance()->getId()) {
        agency.removeValues(boostrapKey, false);
      }
      LOG_TOPIC(DEBUG, Logger::STARTUP)
        << "raceForClusterBootstrap: somebody else does the bootstrap";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
      
    // No value set, we try to do the bootstrap ourselves:
    VPackBuilder b;
    b.add(VPackValue(arangodb::ServerState::instance()->getId()));
    result = agency.casValue(boostrapKey, b.slice(), false, 300, 15);
    if (!result.successful()) {
      LOG_TOPIC(DEBUG, Logger::STARTUP)
      << "raceForClusterBootstrap: lost race, somebody else will bootstrap";
      // Cannot get foot into the door, try again later:
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    // OK, we handle things now
    LOG_TOPIC(DEBUG, Logger::STARTUP)
        << "raceForClusterBootstrap: race won, we do the bootstrap";
    
    // let's see whether a DBserver is there:
    auto dbservers = ci->getCurrentDBServers();
    if (dbservers.size() == 0) {
      LOG_TOPIC(TRACE, Logger::STARTUP)
          << "raceForClusterBootstrap: no DBservers, waiting";
      agency.removeValues(boostrapKey, false);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    auto vocbase = DatabaseFeature::DATABASE->systemDatabase();
    VPackBuilder builder;
    V8DealerFeature::DEALER->loadJavaScriptFileInDefaultContext(
        vocbase, "server/bootstrap/cluster-bootstrap.js", &builder);

    VPackSlice jsresult = builder.slice();
    if (!jsresult.isTrue()) {
      LOG_TOPIC(ERR, Logger::STARTUP) << "Problems with cluster bootstrap, "
        << "marking as not successful.";
      if (!jsresult.isNone()) {
        LOG_TOPIC(ERR, Logger::STARTUP) << "Returned value: "
          << jsresult.toJson();
      } else {
        LOG_TOPIC(ERR, Logger::STARTUP) << "Empty returned value.";
      }
      agency.removeValues(boostrapKey, false);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    
    LOG_TOPIC(DEBUG, Logger::STARTUP) << "Creating the root user";
    AuthenticationFeature::INSTANCE->authInfo()->createRootUser();

    LOG_TOPIC(DEBUG, Logger::STARTUP)
        << "raceForClusterBootstrap: bootstrap done";

    b.clear();
    b.add(VPackValue(arangodb::ServerState::instance()->getId() + ": done"));
    result = agency.setValue(boostrapKey, b.slice(), 0);
    if (result.successful()) {
      return;
    }

    LOG_TOPIC(TRACE, Logger::STARTUP)
        << "raceForClusterBootstrap: could not indicate success";
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void BootstrapFeature::start() {
  auto vocbase = DatabaseFeature::DATABASE->systemDatabase();

  auto ss = ServerState::instance();
  ServerState::RoleEnum role =  ss->getRole();
  if (ServerState::isRunningInCluster(role)) {
    // the coordinators will race to perform the cluster initialization.
    // The coordinatpr who does it will create system collections and
    // the root user
    if (ServerState::isCoordinator(role)) {
      LOG_TOPIC(DEBUG, Logger::STARTUP) << "Racing for cluster bootstrap...";
      raceForClusterBootstrap();
      bool success = false;
      while (!success) {
        LOG_TOPIC(DEBUG, Logger::STARTUP)
        << "Running server/bootstrap/coordinator.js";
        
        VPackBuilder builder;
        V8DealerFeature::DEALER->loadJavaScriptFileInAllContexts(vocbase,
                                                                 "server/bootstrap/coordinator.js", &builder);
        
        auto slice = builder.slice();
        if (slice.isArray()) {
          if (slice.length() > 0) {
            bool newResult = true;
            for (VPackSlice val: VPackArrayIterator(slice)) {
              newResult = newResult && val.isTrue();
            }
            if (!newResult) {
              LOG_TOPIC(ERR, Logger::STARTUP)
              << "result of bootstrap was: " << builder.toJson() << ". retrying bootstrap in 1s.";
            }
            success = newResult;
          } else {
            LOG_TOPIC(ERR, Logger::STARTUP)
            << "bootstrap wasn't executed in a single context! retrying bootstrap in 1s.";
          }
        } else {
          LOG_TOPIC(ERR, Logger::STARTUP)
          << "result of bootstrap was not an array: " << slice.typeName() << ". retrying bootstrap in 1s.";
        }
        if (!success) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      }
    } else if (ServerState::isDBServer(role)) {
      LOG_TOPIC(DEBUG, Logger::STARTUP) << "Running server/bootstrap/db-server.js";
      // only run the JavaScript in V8 context #0.
      V8DealerFeature::DEALER->loadJavaScriptFileInDefaultContext(vocbase, "server/bootstrap/db-server.js", nullptr);
    } else {
      TRI_ASSERT(false);
    }
  } else {

    // become leader before running server.js to ensure the leader
    // is the foxxmaster. Everything else is handled in heartbeat
    if (ServerState::isSingleServer(role) && AgencyCommManager::isEnabled()) {
      std::string const leaderPath = "Plan/AsyncReplication/Leader";
      std::string const myId = ServerState::instance()->getId();
      
      try {
        VPackBuilder myIdBuilder;
        myIdBuilder.add(VPackValue(myId));
        AgencyComm agency;
        AgencyCommResult res = agency.getValues(leaderPath);
        VPackSlice leaderSlice;
        if (res.successful()) {
          leaderSlice = res.slice()[0].get(AgencyCommManager::slicePath(leaderPath));
          if (!leaderSlice.isString() || leaderSlice.getStringLength() == 0) {
            res = agency.casValue(leaderPath, leaderSlice, myIdBuilder.slice(),
                                  /* ttl */ 0, /* timeout */ 5.0);
            if (res.successful()) {
              // sucessfull leadership takeover
              ss->setFoxxmaster(myId);
              LOG_TOPIC(INFO, Logger::STARTUP) << "Became leader in automatic failover setup";
            } // heartbeat thread will take care later
          } else {
            ss->setFoxxmaster(leaderSlice.copyString());
            LOG_TOPIC(INFO, Logger::STARTUP) << "Following " << ss->getFoxxmaster();
          }
        }
      } catch(...) {} // weglaecheln
    }
    
    // will run foxx/manager.js internally and start queues etc
    LOG_TOPIC(DEBUG, Logger::STARTUP) << "Running server/server.js";
    V8DealerFeature::DEALER->loadJavaScriptFileInAllContexts(vocbase, "server/server.js", nullptr);
    // Agency is not allowed to call this
    if (ServerState::isSingleServer(role)) {
      // only creates root user if it does not exist, will be overwritten on slaves
      AuthenticationFeature::INSTANCE->authInfo()->createRootUser();
    }
  }
  
  
  if (ServerState::isSingleServer(role) && AgencyCommManager::isEnabled()) {
    // simon: is set to correct value in the heartbeat thread
    ServerState::setServerMode(ServerState::Mode::TRYAGAIN);
  } else {
    // Start service properly:
    ServerState::setServerMode(ServerState::Mode::DEFAULT);
  }
  
  LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "ArangoDB (version " << ARANGODB_VERSION_FULL
            << ") is ready for business. Have fun!";
  if (_bark) {
    LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "The dog says: wau wau!";
  }

  _isReady = true;
}

void BootstrapFeature::unprepare() {
  // notify all currently running queries about the shutdown
  auto databaseFeature =
      application_features::ApplicationServer::getFeature<DatabaseFeature>(
          "Database");

  if (ServerState::instance()->isCoordinator()) {
    for (auto& id : databaseFeature->getDatabaseIdsCoordinator(true)) {
      TRI_vocbase_t* vocbase = databaseFeature->useDatabase(id);

      if (vocbase != nullptr) {
        vocbase->queryList()->killAll(true);
        vocbase->release();
      }
    }
  } else {
    for (auto& name : databaseFeature->getDatabaseNames()) {
      TRI_vocbase_t* vocbase = databaseFeature->useDatabase(name);

      if (vocbase != nullptr) {
        vocbase->queryList()->killAll(true);
        vocbase->release();
      }
    }
  }
}
