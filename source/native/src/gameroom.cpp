// Game Room: one page for every game: Freedoom, Tyrian, the Quake games and the console games (NES, Game Boy, PS1) (2026-11-20, owner: "can we just have all our games
// in there? Not the NES emulator"). `freedoom-qnx --play` shows the combined game list and runs the Doom games;
// for Tyrian or a Quake game it becomes tyrian-qnx or quake-qnx (exec: same process id), all three in this
// plugin's folder.
//
// One page: Play (action 30: the list), Continue last save (action 31, `--play --continue`: the newest save of
// the Doom and Quake games) and the controls. A push while the page is shown and focused starts the game; the game
// draws on its own screen layer and reads a wired Xbox pad itself, so this plugin never writes to the screen.
// The page hiding (BACK) or losing focus sends "stop" over UDP to whichever engine is running (each listens on its
// own port, so all three are told); the game then takes its layer off and exits. If it is already gone,
// `--restore` takes the layer off instead.
#ifndef HMI_RESCUE_APP
#define HMI_RESCUE_APP "GameRoom"
#endif
#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include "debug_trace.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef FD_PORT
#define FD_PORT 42890
#endif
#ifndef TY_PORT
#define TY_PORT 42891
#endif
#ifndef QK_PORT
#define QK_PORT 42892
#endif
#ifndef NS_PORT
#define NS_PORT 42893
#endif

namespace {
const char kWorker[] = "/var/opt/hmi/data/ID5/Plugins/GameRoom/freedoom-qnx";
const unsigned short kPorts[] = {FD_PORT, TY_PORT, QK_PORT, NS_PORT};   /* Freedoom, Tyrian, Quake, the console emulators */
const unsigned kPage = 100u;
}

class GameRoom : public bmw::PluginApp {
    bool visible_, focused_, started_;
    pid_t game_;
    int ctl_;

    void tell(const char* text) {
        if (ctl_ < 0) return;
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (unsigned i = 0; i < sizeof kPorts / sizeof kPorts[0]; ++i) {
            a.sin_port = htons(kPorts[i]);
            sendto(ctl_, text, strlen(text), 0, reinterpret_cast<struct sockaddr*>(&a), sizeof a);
        }
    }
    bool running() {
        reap_worker(game_);
        return game_ > 0;
    }
    void play(const char* option, const char* what) {
        if (!visible_ || !focused_) {
            trace_event("GameRoom", "play_ignored reason=not_shown_or_focused");
            return;
        }
        if (running()) {
            trace_event("GameRoom", "play_ignored reason=already_running");
            return;
        }
        const char* args[] = {kWorker, "--play", option, 0};
        game_ = trace_launch("GameRoom", this, args, -1);
        started_ = true;
        char line[48];
        snprintf(line, sizeof line, "%s pid=%ld", what, static_cast<long>(game_));
        trace_event("GameRoom", line);
    }
    void stop(const char* why) {
        if (!started_) return;
        char line[64];
        if (running()) {
            tell("stop");
            snprintf(line, sizeof line, "stop reason=%s", why);
        } else {
            const char* args[] = {kWorker, "--restore", 0};
            trace_launch("GameRoom", this, args, -1);
            snprintf(line, sizeof line, "restore reason=%s", why);
        }
        trace_event("GameRoom", line);
        started_ = false;
    }

    bool onPlay(const bmw::Parcel&) {
        trace_event("GameRoom", "play_button");
        play(0, "play");
        return true;
    }
    bool onContinue(const bmw::Parcel&) {
        trace_event("GameRoom", "continue_button");
        play("--continue", "continue");
        return true;
    }
    template <unsigned Id> bool pageEvent(const bmw::Parcel& data) {
        bool on;
        if (data.readBool(on)) {
            if (Id == 1u) focused_ = on; else visible_ = on;
            if (!on) stop(Id == 1u ? "focus_lost" : "hidden");
        }
        return true;
    }

    typedef bmw::Delegate<bool, const bmw::Parcel&> Action;

public:
    GameRoom(bmw::IDispatcher& d, bmw::BasicString<char> name, unsigned maj, unsigned min,
             bmw::RHMIInterfaceVersion version, const bmw::Bundle& cfg, const bmw::Bundle& res)
        : PluginApp(d, name, maj, min, version, cfg, res), visible_(false), focused_(false), started_(false),
          game_(-1), ctl_(-1) {}
    virtual ~GameRoom() {
        stop("destroyed");
        if (ctl_ >= 0) close(ctl_);
        ctl_ = -1;
        trace_stop();
    }
    virtual bool onFinalize() {
        stop("finalize");
        trace_event("GameRoom", "finalize");
        trace_stop();
        return true;
    }
    virtual bool onInit() {
        trace_start(kWorker);
        ctl_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (ctl_ >= 0 && (fcntl(ctl_, F_SETFL, O_NONBLOCK) < 0 || fcntl(ctl_, F_SETFD, FD_CLOEXEC) < 0)) {
            close(ctl_);
            ctl_ = -1;
        }
        Action a;
        a.m_obj = this;
        a.m_exec = &Action::execImpl<GameRoom, &GameRoom::onPlay>;
        addHandler(30, a);
        a.m_exec = &Action::execImpl<GameRoom, &GameRoom::onContinue>;
        addHandler(31, a);
        a.m_exec = &Action::execImpl<GameRoom, &GameRoom::pageEvent<1> >;
        addHandler(kPage, 1, a);
        a.m_exec = &Action::execImpl<GameRoom, &GameRoom::pageEvent<11> >;
        addHandler(kPage, 11, a);
        trace_event("GameRoom", "ready");
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo = {1, "GameRoom", "owner-development", 1, create_app<GameRoom>, destroy_app};
}
