#include "arch/Arch.h"
#include "base/Log.h"
#include "platform/BluetoothBridgeDaemon.h"

int
main(int argc, char** argv)
{
    Arch arch;
    arch.init();

    Log log;
    BluetoothBridgeDaemon app;
    return app.run(argc, argv);
}
