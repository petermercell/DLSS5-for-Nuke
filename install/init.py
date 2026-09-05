"""
DLSS 5 Native Live - Version Router for Foundry Nuke
Appends the correct bin/Nuke<version> directory to nuke.pluginPath.

Nuke's plug-in ABI is not stable across minor releases, so a version-specific
folder (bin/Nuke17.1) is preferred when present and bin/Nuke17 is the fallback.
"""
import os
import nuke

_plugin_dir = os.path.dirname(__file__).replace("\\", "/")

_candidates = [
    "{0}/bin/Nuke{1}.{2}".format(_plugin_dir, nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR),
    "{0}/bin/Nuke{1}".format(_plugin_dir, nuke.NUKE_VERSION_MAJOR),
]

for _version_bin in _candidates:
    if os.path.isdir(_version_bin):
        if _version_bin not in nuke.pluginPath():
            nuke.pluginAddPath(_version_bin)
        break
