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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "Agent.h"

using namespace arangodb::velocypack;
namespace arangodb {
namespace consensus {

Agent::Agent () {}

Agent::Agent (config_t const& config) : _config(config) {
  _constituent.configure(this);
  _log.configure(this);
}

Agent::~Agent () {}

void Agent::start() {
  _constituent.start();
}

term_t Agent::term () const {
  return _constituent.term();
}

bool Agent::vote(id_t id, term_t term) {
	return _constituent.vote(id, term);
}

Config<double> const& Agent::config () const {
  return _config;
}

void Agent::print (arangodb::LoggerStream& logger) const {
  //logger << _config;
}

void Agent::report(status_t status) {
  _status = status;
}

id_t Agent::leaderID () const {
  return _constituent.leaderID();
}

arangodb::LoggerStream& operator<< (arangodb::LoggerStream& l, Agent const& a) {
  a.print(l);
  return l;
}

write_ret_t Agent::write (std::shared_ptr<arangodb::velocypack::Builder> builder) const {
  if (_constituent.leader()) {     // We are leading
    return _log.write (builder);
  } else {                         // We are not
    return write_ret_t(false,_constituent.leaderID());
  }
}

std::shared_ptr<arangodb::velocypack::Builder> builder
  Agent::read (std::shared_ptr<arangodb::velocypack::Builder> builder) const {
  return builder;
}

}}
