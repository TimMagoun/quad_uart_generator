Import("env")

from pathlib import Path
import subprocess


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)


framework_dir = env.PioPlatform().get_package_dir("framework-arduinopico")
if not framework_dir:
    print("[patches] framework-arduinopico package not found")
    env.Exit(1)

project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
patch_dir = project_dir / "patches" / "framework-arduinopico"
patches = sorted(patch_dir.glob("*.patch"))

if not patches:
    print(f"[patches] no patch files found in {patch_dir}")
else:
    for patch in patches:
        patch_path = str(patch.resolve())
        rel = patch.relative_to(project_dir)

        check = run(["git", "apply", "--check", patch_path], framework_dir)
        if check.returncode == 0:
            apply = run(["git", "apply", patch_path], framework_dir)
            if apply.returncode != 0:
                print(f"[patches] failed to apply {rel}")
                print(apply.stderr.strip())
                env.Exit(1)
            print(f"[patches] applied {rel}")
            continue

        reverse = run(["git", "apply", "--check", "--reverse", patch_path], framework_dir)
        if reverse.returncode == 0:
            print(f"[patches] already applied {rel}")
            continue

        print(f"[patches] cannot apply {rel}")
        if check.stderr.strip():
            print(check.stderr.strip())
        env.Exit(1)
