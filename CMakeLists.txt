# Storm Editor: the tool windows of the editor of Storm Engine (engine --editor).
# The engine builds it as one of its modules from src/libs/editor_tools; the windows register
# themselves when the module is linked, and the engine runs the same without it.
if (NOT STORM_EDITOR_TOOLS)
    return()
endif()

STORM_SETUP(
        TARGET_NAME editor_tools
        TYPE storm_module
        DEPENDENCIES core editor model renderer animation
)
# the gizmos of the window Gizmo (MIT, from Conan of the engine)
target_link_libraries(editor_tools PRIVATE imguizmo::imguizmo)
