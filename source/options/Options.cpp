
#include <iostream>
#include <string>
#include <vector>

class Options {
private:
    bool fullScreen;
    int volume;
    std::string videoBackend;

public:
    Options() : fullScreen(false), volume(100), videoBackend("OpenGL") {}

    void setFullScreen(bool enabled) {
        fullScreen = enabled;
    }

    bool isFullScreen() const {
        return fullScreen;
    }

    void setVolume(int value) {
        if (value >= 0 && value <= 100) {
            volume = value;
        }
    }

    int getVolume() const {
        return volume;
    }

    void setVideoBackend(const std::string& backend) {
        videoBackend = backend;
    }

    std::string getVideoBackend() const {
        return videoBackend;
    }
};
