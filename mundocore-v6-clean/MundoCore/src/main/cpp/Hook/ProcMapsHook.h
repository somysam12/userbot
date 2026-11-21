#ifndef PROC_MAPS_HOOK_H
#define PROC_MAPS_HOOK_H

class ProcMapsHook {
public:
    static void setHostDataDir(const char *hostDataDir);
    static void init();
};

#endif //PROC_MAPS_HOOK_H
