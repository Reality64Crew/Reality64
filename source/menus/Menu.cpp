#include <iostream>
#include <string>
#include <vector>

class Menu {
private:
    std::vector<std::string> options;
    int selectedIndex;

public:
    Menu() : selectedIndex(0) {
        options = {
            "Load ROM",
            "Settings",
            "Exit"
        };
    }

    void display() const {
        for (size_t i = 0; i < options.size(); ++i) {
            if (static_cast<int>(i) == selectedIndex) {
                std::cout << "> " << options[i] << "\n";
            } else {
                std::cout << "  " << options[i] << "\n";
            }
        }
    }

    void navigateUp() {
        if (selectedIndex > 0) {
            selectedIndex--;
        }
    }

    void navigateDown() {
        if (selectedIndex < static_cast<int>(options.size()) - 1) {
            selectedIndex++;
        }
    }

    int getSelectedIndex() const {
        return selectedIndex;
    }
};
