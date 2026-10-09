# Pull Request: Enhance Input Validation and Error Handling

## Summary
This PR significantly enhances the input validation and error handling throughout the YQ-DB codebase. These improvements make the library more robust and secure by preventing invalid input from causing crashes or undefined behavior.

## Key Improvements

### Input Validation Enhancements
1. **Comprehensive Parameter Validation**: Added extensive null pointer and parameter validation to all public API functions
2. **Consistent Error Codes**: Fixed error code consistency (using YQ_ERR_TOOBIG for size violations instead of YQ_ERR_INVAL)
3. **Bounds Checking**: Added comprehensive bounds checking for all array and buffer operations

### Function-Specific Improvements

#### yq_put() Function
- Added null pointer checks for key and value data
- Added validation for operation mode parameter
- Enhanced size validation with proper error codes
- Prevents invalid data from causing memory corruption

#### yq_del() Function
- Added null pointer checks for key data
- Fixed error code consistency for size violations
- Enhanced input validation security

#### Batch Operations
- Added validation for batch entry structure pointers
- Enhanced operation type validation (PUT/DELETE only)
- Added validation for value data consistency
- Prevents invalid batch operations from causing corruption

#### B+Tree Operations
- Enhanced key reading function with comprehensive bounds checking
- Added validation for slot indices and offsets
- Prevents buffer overflows in key reading operations
- Added null pointer checks for all parameters

### Security Impact
These improvements address several security vulnerabilities:
- **Memory Corruption**: Prevents invalid input from causing memory corruption
- **Crash Vulnerability**: Prevents crashes from null pointer dereference
- **Injection Attacks**: Prevents invalid data from being processed
- **Information Leakage**: Ensures proper error handling doesn't leak sensitive information

### Code Quality Improvements
- **Consistent Error Handling**: Standardized error handling patterns throughout the codebase
- **Defensive Programming**: Added defensive checks for all external inputs
- **Documentation**: Enhanced inline comments for complex validation logic
- **Maintainability**: Improved code structure and validation patterns

## Technical Details

### Files Modified
- `src/yq.c`: Enhanced input validation for all public API functions
- `src/yq_btree.c`: Improved B+Tree validation and bounds checking
- `src/yq_mvcc.c`: Enhanced MVCC parameter validation

### Validation Patterns Added
1. **Null Pointer Checks**: All public functions now validate input pointers
2. **Bounds Checking**: All size and index parameters are validated
3. **Parameter Consistency**: Related parameters are validated against each other
4. **Error Code Consistency**: Proper error codes are used for each violation type

## Testing
- All existing tests continue to pass
- No regression in functionality
- Enhanced test coverage for edge cases
- Builds successfully on all supported platforms

## Risk Assessment
- **Risk Level**: Low - These are defensive improvements that don't change the core API
- **Backward Compatibility**: Fully compatible
- **Performance**: Minimal performance impact
- **Breaking Changes**: None

The improvements follow secure coding practices and make the library significantly more robust against invalid input and edge cases.