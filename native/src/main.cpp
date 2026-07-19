#include "Application.hpp"
#include "Config.hpp"

#include <SDL3/SDL_log.h>

#include <exception>
#include <utility>

int main(int argc, char** argv) {
    try {
        auto config = terrain::Config::fromArgs(argc, argv);
        terrain::Application app{std::move(config)};
        return app.run();
    } catch (const std::exception& error) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.what());
        return 1;
    }
}
