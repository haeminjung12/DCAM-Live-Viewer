#pragma once
#include <streambuf>
#include <string>
#include <sstream>
#include <QString>

// Tee std::cout/std::cerr to both console and logMessage.
template <typename LoggerFn>
class LogTeeBuf : public std::stringbuf {
public:
    LogTeeBuf(std::streambuf* original, LoggerFn logger)
        : orig(original), logFn(logger) {}

    int sync() override {
        std::string s = str();
        if (!s.empty()) {
            if (orig) orig->sputn(s.data(), s.size());
            QString msg = QString::fromStdString(s);
            while (msg.endsWith('\n') || msg.endsWith('\r')) msg.chop(1);
            if (!msg.isEmpty()) logFn(msg);
        }
        str("");
        return 0;
    }
private:
    std::streambuf* orig;
    LoggerFn logFn;
};
