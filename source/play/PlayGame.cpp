#include <string>
#include <memory>

class PlayGame {
private:
    std::string currentRomPath;
    bool isPaused;

public:
    PlayGame() : isPaused(false) {}

    bool loadGame(const std::string& romPath) {
        currentRomPath = romPath;
        isPaused = false;
        return true;
    }

    void start() {
        if (currentRomPath.empty()) {
            return;
        }
        isPaused = false;
    }

    void pause() {
        isPaused = true;
    }

    void resume() {
        isPaused = false;
    }

    void stop() {
        currentRomPath.clear();
        isPaused = false;
    }

    bool getIsPaused() const {
        return isPaused;
    }

    std::string getCurrentRomPath() const {
        return currentRomPath;
    }
};
