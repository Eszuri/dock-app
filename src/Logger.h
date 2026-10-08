#pragma once
#include <fstream>
#include <string>
#include <windows.h>

inline void Log(const std::string& msg) {
    std::ofstream ofs("D:\\Codingan\\C++\\dock app\\LiteDock.log", std::ios::app);
    if (ofs.is_open()) {
        ofs << "[" << GetTickCount() << "] " << msg << std::endl;
    }
}
