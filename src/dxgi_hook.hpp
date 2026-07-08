#pragma once

class DxgiHook {
public:
    static bool install();
    static void uninstall();
    static bool is_installed();
};
