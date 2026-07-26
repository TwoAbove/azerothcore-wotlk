# mod-bot-minds: needs nlohmann/json (already installed for mod-ollama-bot-buddy)
if(TARGET modules)
    target_include_directories(modules PRIVATE /usr/local/include)
endif()
