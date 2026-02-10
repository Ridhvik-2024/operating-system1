def is_browser_context(state) -> bool:
    return state.active_app in ["browser", "chrome", "msedge"]