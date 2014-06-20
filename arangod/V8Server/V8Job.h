////////////////////////////////////////////////////////////////////////////////
/// @brief V8 job
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Copyright 2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_V8SERVER_V8JOB_H
#define ARANGODB_V8SERVER_V8JOB_H 1

#include "Basics/Common.h"

#include "Dispatcher/Job.h"
#include "BasicsC/json.h"

struct TRI_vocbase_s;

// -----------------------------------------------------------------------------
// --SECTION--                                                       class V8Job
// -----------------------------------------------------------------------------

namespace triagens {
  namespace arango {
    class ApplicationV8;

    class V8Job : public rest::Job {
      private:
        V8Job (V8Job const&) = delete;
        V8Job& operator= (V8Job const&) = delete;

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a new V8 job
////////////////////////////////////////////////////////////////////////////////

        V8Job (struct TRI_vocbase_s*,
               ApplicationV8*,
               std::string const&,
               TRI_json_t const*);

// -----------------------------------------------------------------------------
// --SECTION--                                                       Job methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        JobType type ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        const std::string& queue ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        status_t work ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool cancel (bool running);

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void cleanup ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool beginShutdown ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void handleError (basics::TriagensError const& ex);

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief vocbase
////////////////////////////////////////////////////////////////////////////////

        struct TRI_vocbase_s* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief V8 dealer
////////////////////////////////////////////////////////////////////////////////

        ApplicationV8* _v8Dealer;

////////////////////////////////////////////////////////////////////////////////
/// @brief the command to execute
////////////////////////////////////////////////////////////////////////////////

        std::string const _command;

////////////////////////////////////////////////////////////////////////////////
/// @brief paramaters
////////////////////////////////////////////////////////////////////////////////

        TRI_json_t const* _parameters;

////////////////////////////////////////////////////////////////////////////////
/// @brief cancel flag
////////////////////////////////////////////////////////////////////////////////

        volatile sig_atomic_t _canceled;
    };
  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
