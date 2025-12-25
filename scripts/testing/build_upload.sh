#!/bin/bash
# ESPlan Boiler Controller - Build, Upload and Monitor Script
# Usage: ./build_upload.sh [options]
#   -c    Clean build (removes .pio directory)
#   -u    Upload after build
#   -m    Monitor after upload
#   -a    All (clean, build, upload, monitor) - default
#   -e    Environment (default: esp32dev_usb_debug_selective)
#   -p    Port (default: /dev/ttyACM0)
#   -h    Show help

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
CLEAN=false
BUILD=true
UPLOAD=false
MONITOR=false
ENV="esp32dev_usb_debug_selective"
PORT="/dev/ttyACM0"

# Function to print colored output
print_status() {
    echo -e "${BLUE}[$(date +'%H:%M:%S')]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[$(date +'%H:%M:%S')] ✓${NC} $1"
}

print_error() {
    echo -e "${RED}[$(date +'%H:%M:%S')] ✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[$(date +'%H:%M:%S')] ⚠${NC} $1"
}

# Show usage
show_help() {
    echo "ESPlan Boiler Controller - Build Helper"
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -c    Clean build (removes .pio directory)"
    echo "  -u    Upload after build"
    echo "  -m    Monitor after upload"
    echo "  -a    All (clean, build, upload, monitor) - default"
    echo "  -e    Environment (default: esp32dev_usb_debug_selective)"
    echo "  -p    Port (default: /dev/ttyACM0)"
    echo "  -h    Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 -a                    # Clean, build, upload and monitor"
    echo "  $0 -cu                   # Clean build and upload"
    echo "  $0 -um                   # Upload and monitor (no build)"
    echo "  $0 -e esp32dev_test_debug_selective -a  # Use test environment"
    exit 0
}

# Parse command line arguments
while getopts "cumahe:p:" opt; do
    case $opt in
        c)
            CLEAN=true
            ;;
        u)
            UPLOAD=true
            ;;
        m)
            MONITOR=true
            ;;
        a)
            CLEAN=true
            BUILD=true
            UPLOAD=true
            MONITOR=true
            ;;
        e)
            ENV="$OPTARG"
            ;;
        p)
            PORT="$OPTARG"
            ;;
        h)
            show_help
            ;;
        \?)
            echo "Invalid option: -$OPTARG" >&2
            show_help
            ;;
    esac
done

# If no options specified, default to -a (all)
if [ "$OPTIND" -eq 1 ]; then
    CLEAN=true
    BUILD=true
    UPLOAD=true
    MONITOR=true
fi

# Header
echo ""
echo "=========================================="
echo "   ESPlan Boiler Controller Builder"
echo "=========================================="
echo ""
print_status "Environment: ${GREEN}$ENV${NC}"
print_status "Port: ${GREEN}$PORT${NC}"
echo ""

# Check if PlatformIO is installed
if ! command -v pio &> /dev/null; then
    print_error "PlatformIO not found! Please install it first."
    exit 1
fi

# Clean if requested
if [ "$CLEAN" = true ]; then
    print_status "Cleaning build environment..."
    pio run -t clean -e "$ENV" > /dev/null 2>&1
    rm -rf .pio
    print_success "Clean complete"
fi

# Build
if [ "$BUILD" = true ]; then
    print_status "Building project..."
    echo ""
    
    if pio run -e "$ENV"; then
        echo ""
        print_success "Build successful!"
        
        # Show memory usage
        echo ""
        print_status "Memory usage:"
        pio run -e "$ENV" -t size | grep -E "(RAM|Flash):" | sed 's/^/  /'
    else
        print_error "Build failed!"
        exit 1
    fi
fi

# Upload
if [ "$UPLOAD" = true ]; then
    echo ""
    print_status "Uploading to device on $PORT..."
    
    # Check if port exists
    if [ ! -e "$PORT" ]; then
        print_warning "Port $PORT not found. Waiting for device..."
        
        # Wait up to 10 seconds for device
        for i in {1..10}; do
            if [ -e "$PORT" ]; then
                print_success "Device detected!"
                break
            fi
            sleep 1
        done
        
        if [ ! -e "$PORT" ]; then
            print_error "Device not found on $PORT"
            echo "Available ports:"
            ls /dev/tty* | grep -E "(USB|ACM)" | sed 's/^/  /'
            exit 1
        fi
    fi
    
    if pio run -e "$ENV" -t upload --upload-port "$PORT"; then
        print_success "Upload complete!"
    else
        print_error "Upload failed!"
        exit 1
    fi
fi

# Monitor
if [ "$MONITOR" = true ]; then
    echo ""
    print_status "Starting serial monitor..."
    print_warning "Press Ctrl+C to exit monitor"
    echo ""
    
    # Add a small delay to ensure device is ready
    sleep 1
    
    # Start monitor with the configured settings from platformio.ini
    pio device monitor -p "$PORT" -e "$ENV"
fi

echo ""
print_success "All operations completed!"
echo ""