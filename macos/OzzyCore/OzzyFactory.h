#ifndef OzzyFactory_h
#define OzzyFactory_h

#include "OzzyEngine.h"
#include "../../common/devices/ploytec/ploytec_defs.h"
#include "../Devices/Ploytec/PloytecEngine.h"
#include "../Devices/Dynacord/DynacordEngine.h"

class OzzyFactory {
public:
    static OzzyEngine* CreateEngine(uint16_t vendorID, uint16_t productID) {
        // Dynacord CMS 600-3: Ploytec card under Dynacord's vendor ID
        if (vendorID == DYNACORD_VENDOR_ID && productID == DYNACORD_PID_CMS600_3) {
            return new DynacordEngine(8);
        }

        // Ploytec Devices
        if (vendorID == PLOYTEC_VENDOR_ID) {
            return new PloytecEngine(productID);
        }
        
        // Future: Denon, Pioneer, etc.
        /*
        if (vendorID == 0x154F) {
            return new DenonEngine(productID);
        }
        */
        
        return nullptr;
    }
};

#endif