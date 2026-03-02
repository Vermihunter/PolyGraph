
===== helper to load a plugin and create a Shape =====
struct Plugin {
    void* handle = nullptr;
    create_database_t create = nullptr;
    std::unique_ptr<IDatabase> instance = nullptr;

    bool load(const std::string& path) {
        unload(); // ensure clean state

        handle = dlopen(path.c_str(), RTLD_LAZY);
        if (!handle) {
            std::cerr << "dlopen failed: " << dlerror() << "\n";
            return false;
        }

        create = (create_database_t)dlsym(handle, "create_database");

        if (!create) {
            std::cerr << "Failed loading factory functions\n";
            dlclose(handle);
            handle = nullptr;
            return false;
        }

        instance = create();
        return true;
    }

    void unload() {
        instance = nullptr;

        if (handle) dlclose(handle);
        handle = nullptr;
        create = nullptr;
    }

    ~Plugin() { unload(); }
};
