#!/usr/bin/env python3
"""
Test runner for ESPlan Boiler Controller
Runs both native and embedded tests, collects results, and generates reports
"""

import subprocess
import sys
import os
import time
import json
from datetime import datetime
from pathlib import Path

class TestRunner:
    def __init__(self, project_root=None):
        self.project_root = project_root or Path(__file__).parent.parent
        self.results = {
            'timestamp': datetime.now().isoformat(),
            'native_tests': {},
            'embedded_tests': {},
            'summary': {
                'total_tests': 0,
                'passed': 0,
                'failed': 0,
                'skipped': 0
            }
        }
        
    def run_command(self, command, description):
        """Run a command and capture output"""
        print(f"\n{'='*60}")
        print(f"Running: {description}")
        print(f"Command: {' '.join(command)}")
        print(f"{'='*60}")
        
        start_time = time.time()
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                cwd=self.project_root
            )
            duration = time.time() - start_time
            
            print(result.stdout)
            if result.stderr:
                print("STDERR:", result.stderr)
                
            return {
                'success': result.returncode == 0,
                'duration': duration,
                'stdout': result.stdout,
                'stderr': result.stderr,
                'return_code': result.returncode
            }
        except Exception as e:
            print(f"Error running command: {e}")
            return {
                'success': False,
                'duration': time.time() - start_time,
                'error': str(e)
            }
    
    def parse_unity_output(self, output):
        """Parse Unity test framework output"""
        results = {
            'tests': [],
            'total': 0,
            'passed': 0,
            'failed': 0,
            'ignored': 0
        }
        
        lines = output.split('\n')
        for line in lines:
            # Look for Unity test results in PlatformIO format
            # Format: "test/test_native/test_main.cpp:101: test_name [PASSED]"
            if '.cpp:' in line and ('[PASSED]' in line or '[FAILED]' in line or '[IGNORED]' in line):
                parts = line.split(': ', 2)
                if len(parts) >= 2:
                    # Extract test name and clean it
                    test_parts = parts[1].strip().split('\t')
                    test_name = test_parts[0].strip()
                    
                    if '[PASSED]' in line:
                        results['tests'].append({'name': test_name, 'status': 'PASS'})
                        results['passed'] += 1
                    elif '[FAILED]' in line:
                        # Extract failure message if present
                        fail_msg = ''
                        if len(parts) > 2:
                            fail_parts = parts[2].split('[FAILED]')
                            if len(fail_parts) > 0:
                                fail_msg = fail_parts[0].strip()
                        results['tests'].append({'name': test_name, 'status': 'FAIL', 'message': fail_msg})
                        results['failed'] += 1
                    elif '[IGNORED]' in line:
                        results['tests'].append({'name': test_name, 'status': 'IGNORE'})
                        results['ignored'] += 1
                    results['total'] += 1
            
            # Look for Unity summary line
            # Format: "67 test cases: 2 failed, 64 succeeded"
            if 'test cases:' in line:
                import re
                match = re.search(r'(\d+) test cases?: (\d+) failed, (\d+) succeeded', line)
                if match:
                    results['total'] = int(match.group(1))
                    results['failed'] = int(match.group(2))
                    results['passed'] = int(match.group(3))
                else:
                    # Try alternate format: "66 test cases: 66 succeeded"
                    match = re.search(r'(\d+) test cases?: (\d+) succeeded', line)
                    if match:
                        results['total'] = int(match.group(1))
                        results['passed'] = int(match.group(2))
                        results['failed'] = 0
        
        return results
    
    def run_native_tests(self):
        """Run tests on native platform"""
        print("\n" + "="*60)
        print("RUNNING NATIVE TESTS")
        print("="*60)
        
        # Build native tests
        build_result = self.run_command(
            ['platformio', 'test', '-e', 'native_test', '--without-uploading', '--without-testing'],
            "Building native tests"
        )
        
        if not build_result['success']:
            self.results['native_tests']['build'] = build_result
            return False
        
        # Run native tests
        test_result = self.run_command(
            ['platformio', 'test', '-e', 'native_test', '--without-uploading'],
            "Running native tests"
        )
        
        self.results['native_tests']['test'] = test_result
        
        # Parse results
        if test_result['success']:
            parsed = self.parse_unity_output(test_result['stdout'])
            self.results['native_tests']['parsed'] = parsed
            self.results['summary']['total_tests'] += parsed['total']
            self.results['summary']['passed'] += parsed['passed']
            self.results['summary']['failed'] += parsed['failed']
            self.results['summary']['skipped'] += parsed['ignored']
        
        return test_result['success']
    
    def run_embedded_tests(self, port=None):
        """Run tests on ESP32 hardware"""
        print("\n" + "="*60)
        print("RUNNING EMBEDDED TESTS")
        print("="*60)
        
        if not port:
            print("No port specified for embedded tests, skipping...")
            self.results['embedded_tests']['skipped'] = True
            return True
        
        # Build and upload embedded tests
        build_result = self.run_command(
            ['platformio', 'test', '-e', 'esp32_test', '--upload-port', port, '--without-testing'],
            f"Building and uploading embedded tests to {port}"
        )
        
        if not build_result['success']:
            self.results['embedded_tests']['build'] = build_result
            return False
        
        # Run embedded tests
        test_result = self.run_command(
            ['platformio', 'test', '-e', 'esp32_test', '--upload-port', port],
            "Running embedded tests"
        )
        
        self.results['embedded_tests']['test'] = test_result
        
        # Parse results
        if test_result['success']:
            parsed = self.parse_unity_output(test_result['stdout'])
            self.results['embedded_tests']['parsed'] = parsed
            self.results['summary']['total_tests'] += parsed['total']
            self.results['summary']['passed'] += parsed['passed']
            self.results['summary']['failed'] += parsed['failed']
            self.results['summary']['skipped'] += parsed['ignored']
        
        return test_result['success']
    
    def generate_report(self):
        """Generate test report"""
        print("\n" + "="*60)
        print("TEST REPORT")
        print("="*60)
        
        summary = self.results['summary']
        print(f"\nTotal Tests: {summary['total_tests']}")
        print(f"Passed:      {summary['passed']} ({summary['passed']/max(1, summary['total_tests'])*100:.1f}%)")
        print(f"Failed:      {summary['failed']}")
        print(f"Skipped:     {summary['skipped']}")
        
        # Native tests details
        if 'parsed' in self.results['native_tests']:
            print("\nNative Tests:")
            native = self.results['native_tests']['parsed']
            for test in native['tests']:
                status_symbol = '✓' if test['status'] == 'PASS' else '✗'
                print(f"  {status_symbol} {test['name']} - {test['status']}")
        
        # Embedded tests details
        if 'parsed' in self.results['embedded_tests']:
            print("\nEmbedded Tests:")
            embedded = self.results['embedded_tests']['parsed']
            for test in embedded['tests']:
                status_symbol = '✓' if test['status'] == 'PASS' else '✗'
                print(f"  {status_symbol} {test['name']} - {test['status']}")
        
        # Save detailed report
        report_file = self.project_root / 'test_report.json'
        with open(report_file, 'w') as f:
            json.dump(self.results, f, indent=2)
        print(f"\nDetailed report saved to: {report_file}")
        
        return summary['failed'] == 0
    
    def run_all_tests(self, embedded_port=None):
        """Run all tests and generate report"""
        print(f"Starting test run at {datetime.now()}")
        
        # Check if PlatformIO is installed
        try:
            subprocess.run(['platformio', '--version'], capture_output=True, check=True)
        except:
            print("ERROR: PlatformIO not found. Please install PlatformIO Core.")
            print("Visit: https://docs.platformio.org/en/latest/core/installation.html")
            return False
        
        # Run tests
        native_success = self.run_native_tests()
        embedded_success = self.run_embedded_tests(embedded_port)
        
        # Generate report
        all_passed = self.generate_report()
        
        print("\n" + "="*60)
        if all_passed and native_success and (embedded_success or not embedded_port):
            print("ALL TESTS PASSED! 🎉")
            return True
        else:
            print("SOME TESTS FAILED! ❌")
            return False

def main():
    """Main entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(description='Run tests for ESPlan Boiler Controller')
    parser.add_argument('--port', '-p', help='Serial port for embedded tests (e.g., /dev/ttyUSB0)')
    parser.add_argument('--native-only', action='store_true', help='Run only native tests')
    parser.add_argument('--embedded-only', action='store_true', help='Run only embedded tests')
    
    args = parser.parse_args()
    
    runner = TestRunner()
    
    if args.embedded_only:
        if not args.port:
            print("ERROR: Port required for embedded tests")
            sys.exit(1)
        success = runner.run_embedded_tests(args.port)
        runner.generate_report()
    elif args.native_only:
        success = runner.run_native_tests()
        runner.generate_report()
    else:
        success = runner.run_all_tests(args.port)
    
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()