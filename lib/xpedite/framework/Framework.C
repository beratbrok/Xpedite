////////////////////////////////////////////////////////////////////////////////////////////////
//
// Xpedite frameork control api
//
// Framework initializaion creates a background thread to provide the following functionalities
//   1. Creates a session manager to listen for remote tcp sessions
//   2. Awaits session establishment from local or remote profiler
//   3. Timeshares between, handling of profiler connection and polling for new samples
//   4. Clean up on session disconnect and process shutdown
//
// Author: Manikandan Dhamodharan, Morgan Stanley
//
////////////////////////////////////////////////////////////////////////////////////////////////

#include <xpedite/framework/Framework.H>
#include "session/SessionManager.H"
#include <xpedite/transport/Framer.H>
#include <xpedite/framework/SamplesBuffer.H>
#include <xpedite/log/Log.H>
#include <xpedite/probes/ProbeList.H>
#include <xpedite/util/Tsc.H>
#include <xpedite/common/PromiseKeeper.H>
#include "StorageMgr.H"
#include "request/RequestParser.H"
#include "request/ProbeRequest.H"
#include "request/ProfileRequest.H"
#include <thread>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <unistd.h>
#include <fstream>
#include <chrono>
#include <unistd.h>
#include <atomic>

namespace xpedite { namespace framework {

  class Framework;

  static std::unique_ptr<Framework> instantiateFramework(const char* appInfoFile_, const char* listenerIp_) noexcept;

  class Framework
  {
    public:
      bool isActive();
      void run(std::promise<bool>& sessionInitPromise_, bool awaitProfileBegin_);
      SessionGuard beginProfile(const ProfileInfo& profileInfo_);
      void endProfile();
      bool isRunning() noexcept;
      bool halt() noexcept;

      ~Framework();

    private:

      Framework(const char* appInfoFile_, const char* listenerIp_);

      void handleClient(std::unique_ptr<xpedite::transport::tcp::Socket> clientSocket_, common::PromiseKeeper<bool>& promiseKeeper_) noexcept;
      std::string handleFrame(xpedite::transport::tcp::Frame frame_) noexcept;

      Framework(const Framework&) = delete;
      Framework& operator=(const Framework&) = delete;
      Framework(Framework&&) = default;
      Framework& operator=(Framework&&) = default;

      void log();

      const char* _appInfoPath;
      std::ofstream _appInfoStream;
      session::SessionManager _sessionManager;
      volatile std::atomic<bool> _canRun;

      friend std::unique_ptr<Framework> instantiateFramework(const char* appInfoFile_, const char* listenerIp_) noexcept;
  };

  Framework::Framework(const char* appInfoPath_, const char* listenerIp_)
    : _appInfoPath {appInfoPath_}, _appInfoStream {}, _sessionManager {listenerIp_, 0}, _canRun {true} {
    try {
      _appInfoStream.open(appInfoPath_, std::ios_base::out);
    }
    catch(std::ios_base::failure& e) {
      std::ostringstream stream;
      stream << "xpedite framework init error - failed to open log " << appInfoPath_ << " for writing - " << e.what();
      throw std::runtime_error {stream.str()};
    }
  }

  void Framework::log() {
    static auto tscHz = util::estimateTscHz();
    _appInfoStream << "pid: " << getpid() << std::endl;
    _appInfoStream << "port: " << _sessionManager.listenerPort() << std::endl;
     _appInfoStream<< "binary: " << xpedite::util::getExecutablePath() << std::endl;
     _appInfoStream<< "tscHz: " << tscHz << std::endl;
    log::logProbes(_appInfoStream, probes::probeList());
    _appInfoStream.close();
    XpediteLogInfo << "Xpedite app info stored at - " << _appInfoPath << XpediteLogEnd;
  }

  static std::unique_ptr<Framework> instantiateFramework(const char* appInfoFile_, const char* listenerIp_) noexcept {
    return std::unique_ptr<Framework> {new Framework {appInfoFile_, listenerIp_}};
  }

  void Framework::run(std::promise<bool>& sessionInitPromise_, bool awaitProfileBegin_) {
    common::PromiseKeeper<bool> promiseKeeper {&sessionInitPromise_};

    _sessionManager.start();

    log();

    if(!awaitProfileBegin_) {
      promiseKeeper.deliver(true);
    }

    while(_canRun.load(std::memory_order_relaxed)) {
      _sessionManager.poll();
      if(promiseKeeper.isPending() && _sessionManager.isProfileActive()) {
        promiseKeeper.deliver(true);
      }
      std::this_thread::sleep_for(_sessionManager.pollInterval());
    }

    if(!_canRun.load(std::memory_order_relaxed)) {
      XpediteLogCritical << "xpedite - shutting down handler/thread" << XpediteLogEnd;
      _sessionManager.shutdown();
    }
  }

  SessionGuard Framework::beginProfile(const ProfileInfo& profileInfo_) {
    using namespace xpedite::framework::request;
    ProbeActivationRequest probeActivationRequest {profileInfo_.probes()};
    if(!_sessionManager.execute(&probeActivationRequest)) {
      std::ostringstream stream;
      stream << "xpedite failed to enable probes - " << probeActivationRequest.response().errors();
      XpediteLogCritical <<  stream.str() << XpediteLogEnd;
      return SessionGuard {stream.str()};
    }

    SessionGuard guard {true};
    if(eventCount(&profileInfo_.pmuRequest())) {
      PerfEventsActivationRequest perfEventsRequest {profileInfo_.pmuRequest()};
      if(!_sessionManager.execute(&perfEventsRequest)) {
        std::ostringstream stream;
        stream << "xpedite failed to enable perf events - " << perfEventsRequest.response().errors();
        XpediteLogCritical <<  stream.str() << XpediteLogEnd;
        return SessionGuard {stream.str()};
      }
    }

    ProfileActivationRequest profileActivationRequest {
      StorageMgr::buildSamplesFileTemplate(), MilliSeconds {1}, profileInfo_.samplesDataCapacity()
    };
    if(!_sessionManager.execute(&profileActivationRequest)) {
      std::ostringstream stream;
      stream << "xpedite failed to activate profile - " << profileActivationRequest.response().errors();
      XpediteLogCritical <<  stream.str() << XpediteLogEnd;
      return SessionGuard {stream.str()};
    }
    return std::move(guard);
  }

  void Framework::endProfile() {
    request::ProfileDeactivationRequest profileDeactivationRequest {};
    if(!_sessionManager.execute(&profileDeactivationRequest)) {
      XpediteLogCritical << "xpedite - failed to deactivate profile - " << profileDeactivationRequest.response().errors()
        << XpediteLogEnd;
    }
  }

  Framework::~Framework() {
    if(isRunning()) {
      XpediteLogInfo << "xpedite - framework awaiting thread shutdown, before destruction" << XpediteLogEnd;
      halt();
    }
  }

  bool Framework::isRunning() noexcept {
    return _canRun.load(std::memory_order_relaxed);
  }

  static std::once_flag initFlag;

  static std::thread frameworkThread {};

  static std::unique_ptr<Framework> framework {};

  bool Framework::halt() noexcept {
    auto isRunning = _canRun.exchange(false, std::memory_order_relaxed);
    if(isRunning) {
      XpediteLogInfo << "xpedite - framework awaiting thread shutdown" << XpediteLogEnd;
      frameworkThread.join();
    }
    return isRunning;
  }

  bool initializeThread() {
    static __thread bool threadInitFlag {};
    if(!threadInitFlag) {
      auto tid = util::gettid();
      XpediteLogInfo << "xpedite - initializing framework for thread - " << tid << XpediteLogEnd;
      SamplesBuffer::expand();
      threadInitFlag = true;
      return true;
    }
    return false;
  }

  static void initializeOnce(const char* appInfoFile_, const char* listenerIp_, bool awaitProfileBegin_, bool* rc_) noexcept {
    std::promise<bool> sessionInitPromise;
    std::future<bool> listenerInitFuture = sessionInitPromise.get_future();
    std::thread thread {
      [&sessionInitPromise, appInfoFile_, listenerIp_, awaitProfileBegin_] {
        try {
          framework = instantiateFramework(appInfoFile_, listenerIp_);
          framework->run(sessionInitPromise, awaitProfileBegin_);
        }
        catch(const std::exception& e) {
          XpediteLogCritical << "xpedite - init failed - " << e.what() << XpediteLogEnd;
        }
        catch(...) {
          XpediteLogCritical << "xpedite - init failed - unknown failure" << XpediteLogEnd;
        }
      }
    };
    frameworkThread = std::move(thread);

    // longer timeout, if the framework is awaiting perf client to begin profile
    auto timeout = awaitProfileBegin_ ? 120 : 5;
    if(listenerInitFuture.wait_until(std::chrono::system_clock::now() + std::chrono::seconds(timeout)) != std::future_status::ready) {
      XpediteLogCritical << "xpedite - init failure - failed to start listener (timedout)" << XpediteLogEnd;
      *rc_ = false;
      return;
    }
    *rc_ = true; 
  }

  bool initialize(const char* appInfoFile_, const char* listenerIp_, bool awaitProfileBegin_) {
    initializeThread();
    bool rc {};
    std::call_once(initFlag, initializeOnce, appInfoFile_, listenerIp_, awaitProfileBegin_, &rc);
    return rc;
  }

  bool initialize(const char* appInfoFile_, bool awaitProfileBegin_) {
    return initialize(appInfoFile_, "", awaitProfileBegin_);
  }

  SessionGuard profile(const ProfileInfo& profileInfo_) {
    if(framework) {
      return framework->beginProfile(profileInfo_);
    }
    return {};
  }

  SessionGuard::~SessionGuard() {
    if(_isAlive && framework) {
      XpediteLogInfo << "Live session guard being destroyed - end active profile session" << XpediteLogEnd;
      _isAlive = {};
      framework->endProfile();
    }
  }

  bool isRunning() noexcept {
    if(framework) {
      return framework->isRunning();
    }
    return false;
  }

  void pinThread(unsigned core_) {
    if(isRunning()) {
      util::pinThread(frameworkThread.native_handle(), core_);
      return;
    }
    throw std::runtime_error {"xpedite framework not initialized - no thread to pin"};
  }

  bool halt() noexcept {
    if(framework) {
      return framework->halt();
    }
    return false;
  }

}}
