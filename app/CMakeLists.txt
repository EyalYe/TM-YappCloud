# app_yapp — a self-registering TaskMaster app component (direct-Todoist source).
# REQUIRES taskmaster_core (the stable app API; core is local in the build).
# WHOLE_ARCHIVE keeps the self-registration constructors (TASKMASTER_REGISTER_APP
# + TASKMASTER_REGISTER_APP_CONFIG) from being dropped by --gc-sections.
idf_component_register(
    SRCS "app_yapp.c"
    REQUIRES taskmaster_core
    WHOLE_ARCHIVE
)
