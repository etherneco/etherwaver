#include "arch/Arch.h"
#include "base/Log.h"
#include "platform/LinuxInputRouterDaemon.h"

int
main(int argc, char** argv)
{
    Arch arch;
    arch.init();

    Log log;
    LinuxInputRouterDaemon app;
    return app.run(argc, argv);
}
