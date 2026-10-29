// monopoly - ReXGlue Recompiled Project

#include "generated/default/monopoly_init.h"

#include "monopoly_app.h"

REXCVAR_DEFINE_STRING(input_script, "", "Monopoly", "Replay timed controller presses from this file (testing)");
REXCVAR_DEFINE_BOOL(net_host, false, "Online", "Host an online game: remote players join with monopoly_net");
REXCVAR_DEFINE_INT32(net_port, 27360, "Online", "TCP port to host on (forward it on your router)");
REXCVAR_DEFINE_STRING(net_name, "Host", "Online", "Your name in the online lobby");

REX_DEFINE_APP(monopoly, MonopolyApp::Create)
