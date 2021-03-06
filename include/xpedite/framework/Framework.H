///////////////////////////////////////////////////////////////////////////////////////////////
//
// Xpedite frameork control api
//
// To enable profiling, the app needs to invoke on the initialize() methods.
// As part of initialization, Xpedite will listen for incoming tcp connections from profilers. 
// The application can be profiled by, only one profiler at a time.
//
// Initialization also spawn a background thread to provide the following services
//  1. Accept new tcp connections from profiler
//  2. Process commands to query/update probe and pmc state
//  3. Collection of counted data, from probes in application threads, using a wait free buffer.
//
// The framework can be shutdown by calling halt()
//
// Author: Manikandan Dhamodharan, Morgan Stanley
//
///////////////////////////////////////////////////////////////////////////////////////////////

#pragma once
#include <xpedite/framework/ProfileInfo.H>
#include <algorithm>
#include <string>

namespace xpedite { namespace framework {

  bool initialize(const char* appInfoFile_, bool awaitProfileBegin_ = {});

  bool initialize(const char* appInfoFile_, const char* listenerIp_, bool awaitProfileBegin_ = {});

  bool initializeThread();

  bool isRunning() noexcept;

  void pinThread(unsigned core_);

  bool halt() noexcept;

  class Framework;
  class SessionGuard
  {
    bool _isAlive;
    std::string _errors;

    friend class Framework;

    explicit SessionGuard(bool isAlive_) 
      : _isAlive {isAlive_}, _errors {} {
    }

    explicit SessionGuard(std::string errors_) 
      : _isAlive {}, _errors {errors_} {
    }

    public:

    SessionGuard()
      : SessionGuard {std::string {"Invalid Session"}} {
    }

    SessionGuard(const SessionGuard&)            = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;

    SessionGuard(SessionGuard&& other_)
      : _isAlive  {other_._isAlive}, _errors {std::move(other_._errors)} {
      other_._isAlive = {};
      other_._errors = "Invalid Session";
    }

    SessionGuard& operator=(SessionGuard&& other_) {
      std::swap(_isAlive, other_._isAlive);
      std::swap(_errors, other_._errors);
      return *this;
    }

    explicit operator bool() const noexcept {
      return _isAlive;
    }

    const std::string& errors() const noexcept {
      return _errors;
    }

    ~SessionGuard();
  };

  SessionGuard profile(const ProfileInfo& profileInfo_);

}}
