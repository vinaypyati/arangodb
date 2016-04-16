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

#include "ClusterFeature.h"

#include "Basics/FileUtils.h"
#include "Basics/JsonHelper.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/files.h"
#include "Cluster/ClusterComm.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/HeartbeatThread.h"
#include "Cluster/ServerState.h"
#include "Dispatcher/DispatcherFeature.h"
#include "Endpoint/Endpoint.h"
#include "Logger/Logger.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "RestServer/DatabaseFeature.h"
#include "SimpleHttpClient/ConnectionManager.h"
#include "VocBase/server.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

ClusterFeature::ClusterFeature(application_features::ApplicationServer* server)
    : ApplicationFeature(server, "Cluster"),
      _username("root"),
      _enableCluster(false),
      _heartbeatThread(nullptr),
      _heartbeatInterval(0),
      _disableHeartbeat(false),
      _agencyCallbackRegistry(nullptr) {
  setOptional(false);
  requiresElevatedPrivileges(false);
  startsAfter("Database");
  startsAfter("Dispatcher");
  startsAfter("Scheduler");
}

ClusterFeature::~ClusterFeature() {
  delete _heartbeatThread;

  // delete connection manager instance
  auto cm = httpclient::ConnectionManager::instance();
  delete cm;
}

void ClusterFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::collectOptions";

  options->addSection("cluster", "Configure the cluster");

  options->addOption("--cluster.agency-endpoint",
                     "agency endpoint to connect to",
                     new VectorParameter<StringParameter>(&_agencyEndpoints));

  options->addOption("--cluster.agency-prefix", "agency prefix",
                     new StringParameter(&_agencyPrefix));

  options->addOption("--cluster.my-local-info", "this server's local info",
                     new StringParameter(&_myLocalInfo));

  options->addOption("--cluster.my-id", "this server's id",
                     new StringParameter(&_myId));

  options->addOption("--cluster.my-role", "this server's role",
                     new StringParameter(&_myRole));

  options->addOption("--cluster.my-address", "this server's endpoint",
                     new StringParameter(&_myAddress));

  options->addOption("--cluster.username",
                     "username used for cluster-internal communication",
                     new StringParameter(&_username));

  options->addOption("--cluster.password",
                     "password used for cluster-internal communication",
                     new StringParameter(&_password));

  options->addOption("--cluster.data-path",
                     "path to cluster database directory",
                     new StringParameter(&_dataPath));

  options->addOption("--cluster.log-path",
                     "path to log directory for the cluster",
                     new StringParameter(&_logPath));

  options->addOption("--cluster.arangod-path",
                     "path to the arangod for the cluster",
                     new StringParameter(&_arangodPath));

  options->addOption("--cluster.dbserver-config",
                     "path to the DBserver configuration",
                     new StringParameter(&_dbserverConfig));

  options->addOption("--cluster.coordinator-config",
                     "path to the coordinator configuration",
                     new StringParameter(&_coordinatorConfig));
}

void ClusterFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::validateOptions";

  // check if the cluster is enabled
  _enableCluster = !_agencyEndpoints.empty();

  if (!_enableCluster) {
    ServerState::instance()->setRole(ServerState::ROLE_SINGLE);
    return;
  }

  // validate --cluster.agency-endpoint (currently a noop)
  if (_agencyEndpoints.empty()) {
    LOG(FATAL)
        << "must at least specify one endpoint in --cluster.agency-endpoint";
    FATAL_ERROR_EXIT();
  }

  // validate
  if (_agencyPrefix.empty()) {
    _agencyPrefix = "arango";
  }

  // validate --cluster.agency-prefix
  size_t found = _agencyPrefix.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/");

  if (found != std::string::npos || _agencyPrefix.empty()) {
    LOG(FATAL) << "invalid value specified for --cluster.agency-prefix";
    FATAL_ERROR_EXIT();
  }

  // validate --cluster.my-id
  if (_myId.empty()) {
    if (_myLocalInfo.empty()) {
      LOG(FATAL) << "Need to specify a local cluster identifier via "
                    "--cluster.my-local-info";
      FATAL_ERROR_EXIT();
    }

    if (_myAddress.empty()) {
      LOG(FATAL)
          << "must specify --cluster.my-address if --cluster.my-id is empty";
      FATAL_ERROR_EXIT();
    }
  } else {
    size_t found = _myId.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");

    if (found != std::string::npos) {
      LOG(FATAL) << "invalid value specified for --cluster.my-id";
      FATAL_ERROR_EXIT();
    }
  }
}

void ClusterFeature::prepare() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::prepare";

  ServerState::instance()->setAuthentication(_username, _password);
  ServerState::instance()->setDataPath(_dataPath);
  ServerState::instance()->setLogPath(_logPath);
  ServerState::instance()->setArangodPath(_arangodPath);
  ServerState::instance()->setDBserverConfig(_dbserverConfig);
  ServerState::instance()->setCoordinatorConfig(_coordinatorConfig);

  // create callback registery
  _agencyCallbackRegistry.reset(
      new AgencyCallbackRegistry(agencyCallbacksPath()));

  // initialize ConnectionManager library
  httpclient::ConnectionManager::initialize();

  // create an instance (this will not yet create a thread)
  ClusterComm::instance();
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
#warning TODO split into methods
#endif

void ClusterFeature::start() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << name() << "::start";

  // initialize ClusterComm library, must call initialize only once
  ClusterComm::initialize();

  // return if cluster is disabled
  if (!_enableCluster) {
    return;
  }

  ServerState::instance()->setClusterEnabled();

  // register the prefix with the communicator
  AgencyComm::setPrefix(_agencyPrefix);

  for (size_t i = 0; i < _agencyEndpoints.size(); ++i) {
    std::string const unified = Endpoint::unifiedForm(_agencyEndpoints[i]);

    if (unified.empty()) {
      LOG(FATAL) << "invalid endpoint '" << _agencyEndpoints[i]
                 << "' specified for --cluster.agency-endpoint";
      FATAL_ERROR_EXIT();
    }

    AgencyComm::addEndpoint(unified);
  }

  // Now either _myId is set properly or _myId is empty and _myLocalInfo and
  // _myAddress are set.
  if (!_myAddress.empty()) {
    ServerState::instance()->setAddress(_myAddress);
  }

  // disable error logging for a while
  ClusterComm::instance()->enableConnectionErrorLogging(false);

  // perform an initial connect to the agency
  std::string const endpoints = AgencyComm::getEndpointsString();

  if (!AgencyComm::initialize()) {
    LOG(FATAL) << "Could not connect to agency endpoints (" << endpoints << ")";
    FATAL_ERROR_EXIT();
  }

  ServerState::instance()->setLocalInfo(_myLocalInfo);

  if (!_myId.empty()) {
    ServerState::instance()->setId(_myId);
  }

  if (!_myRole.empty()) {
    ServerState::RoleEnum role = ServerState::stringToRole(_myRole);

    if (role == ServerState::ROLE_SINGLE ||
        role == ServerState::ROLE_UNDEFINED) {
      LOG(FATAL) << "Invalid role provided. Possible values: PRIMARY, "
                    "SECONDARY, COORDINATOR";
      FATAL_ERROR_EXIT();
    }

    if (!ServerState::instance()->registerWithRole(role)) {
      LOG(FATAL) << "Couldn't register at agency.";
      FATAL_ERROR_EXIT();
    }
  }

  ServerState::RoleEnum role = ServerState::instance()->getRole();

  if (role == ServerState::ROLE_UNDEFINED) {
    // no role found
    LOG(FATAL) << "unable to determine unambiguous role for server '" << _myId
               << "'. No role configured in agency (" << endpoints << ")";
    FATAL_ERROR_EXIT();
  }

  if (role == ServerState::ROLE_SINGLE) {
    LOG(FATAL) << "determined single-server role for server '" << _myId
               << "'. Please check the configurarion in the agency ("
               << endpoints << ")";
    FATAL_ERROR_EXIT();
  }

  if (_myId.empty()) {
    _myId = ServerState::instance()->getId();  // has been set by getRole!
  }

  // check if my-address is set
  if (_myAddress.empty()) {
    // no address given, now ask the agency for our address
    _myAddress = ServerState::instance()->getAddress();
  }

  // if nonempty, it has already been set above

  // If we are a coordinator, we wait until at least one DBServer is there,
  // otherwise we can do very little, in particular, we cannot create
  // any collection:
  if (role == ServerState::ROLE_COORDINATOR) {
    ClusterInfo* ci = ClusterInfo::instance();

    while (true) {
      LOG(INFO) << "Waiting for a DBserver to show up...";
      ci->loadCurrentDBServers();
      std::vector<ServerID> DBServers = ci->getCurrentDBServers();
      if (!DBServers.empty()) {
        LOG(INFO) << "Found a DBserver.";
        break;
      }

      sleep(1);
    };
  }

  if (_myAddress.empty()) {
    LOG(FATAL) << "unable to determine internal address for server '" << _myId
               << "'. Please specify --cluster.my-address or configure the "
                  "address for this server in the agency.";
    FATAL_ERROR_EXIT();
  }

  // now we can validate --cluster.my-address
  std::string const unified = Endpoint::unifiedForm(_myAddress);

  if (unified.empty()) {
    LOG(FATAL) << "invalid endpoint '" << _myAddress
               << "' specified for --cluster.my-address";
    FATAL_ERROR_EXIT();
  }

  ServerState::instance()->setState(ServerState::STATE_STARTUP);

  // the agency about our state
  AgencyComm comm;
  comm.sendServerState(0.0);

  std::string const version = comm.getVersion();

  ServerState::instance()->setInitialized();

  LOG(INFO) << "Cluster feature is turned on. Agency version: " << version
            << ", Agency endpoints: " << endpoints << ", server id: '" << _myId
            << "', internal address: " << _myAddress
            << ", role: " << ServerState::roleToString(role);

  if (!_disableHeartbeat) {
    AgencyCommResult result = comm.getValues("Sync/HeartbeatIntervalMs", false);

    if (result.successful()) {
      result.parse("", false);

      std::map<std::string, AgencyCommResultEntry>::const_iterator it =
          result._values.begin();

      if (it != result._values.end()) {
        VPackSlice slice = (*it).second._vpack->slice();
        _heartbeatInterval =
            arangodb::basics::VelocyPackHelper::stringUInt64(slice);

        LOG(INFO) << "using heartbeat interval value '" << _heartbeatInterval
                  << " ms' from agency";
      }
    }

    // no value set in agency. use default
    if (_heartbeatInterval == 0) {
      _heartbeatInterval = 5000;  // 1/s

      LOG(WARN) << "unable to read heartbeat interval from agency. Using "
                   "default value '"
                << _heartbeatInterval << " ms'";
    }

    // start heartbeat thread
    _heartbeatThread = new HeartbeatThread(DatabaseFeature::DATABASE->server(),
                                           _agencyCallbackRegistry.get(),
                                           _heartbeatInterval * 1000, 5);

    if (_heartbeatThread == nullptr) {
      LOG(FATAL) << "unable to start cluster heartbeat thread";
      FATAL_ERROR_EXIT();
    }

    if (!_heartbeatThread->init() || !_heartbeatThread->start()) {
      LOG(FATAL) << "heartbeat could not connect to agency endpoints ("
                 << endpoints << ")";
      FATAL_ERROR_EXIT();
    }

    while (!_heartbeatThread->isReady()) {
      // wait until heartbeat is ready
      usleep(10000);
    }
  }

  AgencyCommResult result;

  while (true) {
    AgencyCommLocker locker("Current", "WRITE");
    bool success = locker.successful();

    if (success) {
      VPackBuilder builder;
      try {
        VPackObjectBuilder b(&builder);
        builder.add("endpoint", VPackValue(_myAddress));
      } catch (...) {
        locker.unlock();
        LOG(FATAL) << "out of memory";
        FATAL_ERROR_EXIT();
      }

      result = comm.setValue("Current/ServersRegistered/" + _myId,
                             builder.slice(), 0.0);
    }

    if (!result.successful()) {
      locker.unlock();
      LOG(FATAL) << "unable to register server in agency: http code: "
                 << result.httpCode() << ", body: " << result.body();
      FATAL_ERROR_EXIT();
    }

    if (success) {
      break;
    }

    sleep(1);
  }

  if (role == ServerState::ROLE_COORDINATOR) {
    ServerState::instance()->setState(ServerState::STATE_SERVING);
  } else if (role == ServerState::ROLE_PRIMARY) {
    ServerState::instance()->setState(ServerState::STATE_SERVINGASYNC);
  } else if (role == ServerState::ROLE_SECONDARY) {
    ServerState::instance()->setState(ServerState::STATE_SYNCING);
  }

  DispatcherFeature* dispatcher = dynamic_cast<DispatcherFeature*>(
      ApplicationServer::lookupFeature("Dispatcher"));

  dispatcher->buildAqlQueue();
}

void ClusterFeature::stop() {
  if (_enableCluster) {
    if (_heartbeatThread != nullptr) {
      _heartbeatThread->beginShutdown();
    }

    // change into shutdown state
    ServerState::instance()->setState(ServerState::STATE_SHUTDOWN);

    AgencyComm comm;
    comm.sendServerState(0.0);
  }

  ClusterComm::cleanup();

  if (!_enableCluster) {
    return;
  }

  // change into shutdown state
  ServerState::instance()->setState(ServerState::STATE_SHUTDOWN);

  AgencyComm comm;
  comm.sendServerState(0.0);

  {
    // Try only once to unregister because maybe the agencycomm
    // is shutting down as well...
    AgencyCommLocker locker("Current", "WRITE", 120.0, 0.001);

    if (locker.successful()) {
      // unregister ourselves
      ServerState::RoleEnum role = ServerState::instance()->getRole();

      if (role == ServerState::ROLE_PRIMARY) {
        comm.removeValues("Current/DBServers/" + _myId, false);
      } else if (role == ServerState::ROLE_COORDINATOR) {
        comm.removeValues("Current/Coordinators/" + _myId, false);
      }

      // unregister ourselves
      comm.removeValues("Current/ServersRegistered/" + _myId, false);
    }
  }

  while (_heartbeatThread->isRunning()) {
    usleep(50000);
  }

  // ClusterComm::cleanup();
  AgencyComm::cleanup();
}
