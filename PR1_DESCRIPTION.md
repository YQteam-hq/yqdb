# Pull Request: Fix Critical Security and Memory Issues

## Summary
This PR addresses several critical security vulnerabilities and memory leaks discovered in the YQ-DB codebase. These fixes improve the robustness and security of the key-value storage engine.

## Issues Fixed

### Security Vulnerabilities
1. **Null Pointer Dereference**: Fixed missing null pointer checks in `yq_open()` function that could lead to crashes
2. **Integer Overflow**: Added overflow protection in pending capacity calculations and batch operations
3. **Resource Leaks**: Fixed file handle leaks in Windows VFS operations

### Memory Issues
1. **Memory Leaks**: Fixed memory leaks in `pending_push()` when second allocation fails
2. **Resource Management**: Improved error handling to prevent resource leaks in various failure scenarios

### Data Integrity
1. **B+Tree Operations**: Fixed page count initialization logic that could lead to data corruption
2. **Path Validation**: Added named constant for B+Tree path depth limit to prevent stack overflow

## Code Changes

### Files Modified
- `src/yq.c`: Added null checks, overflow protection, improved error handling
- `src/yq_btree.c`: Fixed page count logic, added null checks, added named constant
- `src/yq_vfs.c`: Fixed resource leak in Windows file mapping
- `src/yq_wal.c`: Removed duplicate definition

### Key Changes
1. **Enhanced Input Validation**: Added comprehensive null pointer checks
2. **Memory Safety**: Fixed memory leaks in allocation failure scenarios
3. **Integer Overflow Protection**: Added bounds checking for size calculations
4. **Resource Management**: Proper cleanup in error paths
5. **Code Quality**: Removed duplicate code and added named constants

## Testing
- All basic tests pass
- No regression in existing functionality
- Builds successfully on both Unix and Windows platforms

## Impact
These fixes address critical issues that could lead to:
- Application crashes in production environments
- Memory leaks in high-load scenarios
- Data corruption in edge cases
- Security vulnerabilities from improper input validation

## Risk Assessment
- **Risk Level**: Low - These are defensive fixes that don't change the core API
- **Backward Compatibility**: Fully compatible
- **Performance**: No performance impact
- **Breaking Changes**: None

The fixes follow defensive programming practices and maintain the existing API contract while improving the robustness of the library.