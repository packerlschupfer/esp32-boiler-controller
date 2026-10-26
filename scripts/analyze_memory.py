#!/usr/bin/env python3
"""
Memory usage analyzer for different build configurations
"""

import subprocess
import re
import sys

def get_binary_size(env_name):
    """Build and get size information for an environment"""
    print(f"\nBuilding {env_name}...")
    
    # Build the environment
    result = subprocess.run(
        ["pio", "run", "-e", env_name],
        capture_output=True,
        text=True
    )
    
    if result.returncode != 0:
        print(f"Build failed for {env_name}")
        return None
    
    # Get size information
    result = subprocess.run(
        ["pio", "run", "-e", env_name, "-t", "size"],
        capture_output=True,
        text=True
    )
    
    # Parse size output
    lines = result.stdout.split('\n')
    for line in lines:
        if '.elf' in line and 'text' not in line:
            parts = line.split()
            if len(parts) >= 5:
                return {
                    'text': int(parts[0]),
                    'data': int(parts[1]),
                    'bss': int(parts[2]),
                    'total': int(parts[3])
                }
    
    return None

def format_bytes(size):
    """Format bytes in human readable format"""
    for unit in ['B', 'KB', 'MB']:
        if size < 1024.0:
            return f"{size:.1f} {unit}"
        size /= 1024.0
    return f"{size:.1f} GB"

def main():
    environments = [
        "esp32dev_test_release",
        "esp32dev_no_logger_release",
        "esp32dev_optimized_release"  # If exists
    ]
    
    results = {}
    
    for env in environments:
        size_info = get_binary_size(env)
        if size_info:
            results[env] = size_info
    
    # Display results
    print("\n" + "="*80)
    print("MEMORY USAGE COMPARISON")
    print("="*80)
    print(f"{'Environment':<30} {'Flash (text+data)':<20} {'RAM (data+bss)':<20} {'Total':<15}")
    print("-"*80)
    
    baseline = None
    for env, info in results.items():
        flash = info['text'] + info['data']
        ram = info['data'] + info['bss']
        total = info['total']
        
        print(f"{env:<30} {format_bytes(flash):<20} {format_bytes(ram):<20} {format_bytes(total):<15}")
        
        if baseline is None:
            baseline = info
        else:
            # Show savings compared to baseline
            flash_diff = baseline['text'] + baseline['data'] - flash
            ram_diff = baseline['data'] + baseline['bss'] - ram
            total_diff = baseline['total'] - total
            
            if flash_diff != 0 or ram_diff != 0:
                print(f"{'  Savings:':<30} {format_bytes(flash_diff):<20} {format_bytes(ram_diff):<20} {format_bytes(total_diff):<15}")
    
    print("\nNote: RAM usage shown is static allocation only. Heap usage varies at runtime.")

if __name__ == "__main__":
    main()