Continue Coast development in this source checkout on main.
Repository: https://github.com/camden-pina/ModernOS.git.
Read Documentation/AI-HANDOFF.md, README.md and Documentation/source-builds.md.
Use make iso to build the complete OS and make run to boot Coast.iso.
Use make menuconfig for saved build/VM settings in .config; CONFIG=path selects
another settings file and command-line variables override saved values.
Keep every guide consistent with this workflow; optional arguments control
build resources, output paths, verification and VM settings.
Preserve selected images, usable source build outputs, signing keys and active
package/community services. Keep generated artifacts and local notes out of Git.
Run the appropriate source/host and actual guest checks for each change.
