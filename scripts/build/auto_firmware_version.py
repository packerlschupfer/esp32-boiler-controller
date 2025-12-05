import subprocess
import os
from pathlib import Path

Import("env")

def get_firmware_specifier_build_flag():
    """Get firmware version from git or fallback to PROJECT_VERSION"""
    try:
        # Check if we're in a git repository
        git_dir = Path(".git")
        if not git_dir.exists():
            # Fallback to PROJECT_VERSION from build flags
            print("Not in git repository, using PROJECT_VERSION")
            return None
            
        # Get git describe output
        ret = subprocess.run(
            ["git", "describe", "--always"], 
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE,
            text=True,
            check=False
        )
        
        if ret.returncode == 0:
            build_version = ret.stdout.strip()
            if build_version:
                build_flag = f'-D AUTO_VERSION=\\"{build_version}\\"'
                print(f"Firmware Revision: {build_version}")
                return build_flag
        else:
            print(f"Git describe failed: {ret.stderr}")
            
    except FileNotFoundError:
        print("Git not found in PATH")
    except Exception as e:
        print(f"Error getting version: {e}")
    
    # Fallback - let PROJECT_VERSION be used
    print("Using PROJECT_VERSION as fallback")
    return None

# Only append if we got a valid flag
flag = get_firmware_specifier_build_flag()
if flag:
    env.Append(BUILD_FLAGS=[flag])