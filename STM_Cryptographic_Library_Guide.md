# STM Cryptographic Library Guide
## How to Use SHA256 Hashing with STM32

---

## 📋 Table of Contents
1. [Overview](#overview)
2. [Library Setup](#library-setup)
3. [Key Concepts](#key-concepts)
4. [SHA256 Hashing Methods](#sha256-hashing-methods)
5. [Complete Code Example](#complete-code-example)
6. [Error Handling](#error-handling)
7. [Tips & Best Practices](#tips--best-practices)

---

## Overview

The **STM Cryptographic Library** (CMOX) is a lightweight, embedded cryptographic library designed for STM32 microcontrollers. It provides:

- **Hash functions**: SHA256, SHA384, SHA512, SHA1, MD5
- **Encryption**: AES, ChaCha20
- **Asymmetric**: RSA, ECDSA
- **Easy integration** with STM32 development tools

This guide focuses on **SHA256 hashing** - the most commonly used hash function for data integrity verification.

---

## Library Setup

### 1. **Include Required Headers**

**What you need:**
```c
#include "cmox_crypto.h"
```
- **What it does**: Gives you access to all cryptographic functions (hash, encrypt, decrypt)
- **How it helps**: Declares SHA256, AES, RSA and other algorithms you can use

**Also need:**
```c
#include <string.h>
```
- **What it does**: Provides the `memcmp()` function to compare hashes
- **Why**: You'll need to verify if your computed hash matches an expected hash

---

### 2. **Initialize the Library**

**The setup line:**
```c
cmox_initialize(&init_target)
```
- **What it does**: Prepares the cryptographic library to run
- **How it works**: Sets up internal structures, memory, and hardware accelerators
- **Why needed**: Without this, all crypto functions will fail
- **Important**: Must be called ONCE at program startup, before any crypto operations

**Real example:**
```c
cmox_init_arg_t init_target = {CMOX_INIT_TARGET_AUTO, NULL};
if (cmox_initialize(&init_target) != CMOX_INIT_SUCCESS)
    Error_Handler();
```
- `CMOX_INIT_TARGET_AUTO` = Let the library auto-detect the best hardware mode
- Check the return value to ensure initialization worked

---

### 3. **Clean Up When Done**

**The cleanup line:**
```c
cmox_finalize(NULL)
```
- **What it does**: Shuts down the cryptographic library and frees resources
- **When to call**: Once at the very end of your program
- **Why important**: Prevents memory leaks and resource exhaustion
- **Note**: Pass `NULL` as parameter (it's not currently used)

---

## Key Concepts

### What is SHA256?
- **SHA256** = Secure Hash Algorithm 256-bit
- Creates a **fixed-size 32-byte (256-bit) fingerprint** of any data
- Used for: password hashing, data integrity checks, digital signatures
- **Output size**: Always 32 bytes, regardless of input size

### Two Usage Approaches

#### **Method 1: Single Call** (Simple)
- Best for: Small data, no need for chunking
- Function: `cmox_hash_compute()`
- Hashes entire message at once

#### **Method 2: Multiple Calls** (Flexible)
- Best for: Large data, streaming, memory constraints
- Functions: `cmox_sha256_construct()`, `cmox_hash_init()`, `cmox_hash_append()`, `cmox_hash_generateTag()`
- Process data in chunks

---

## SHA256 Hashing Methods

### Method 1: Single Call (Simple)

**Best for**: Quick hashing of small to medium-sized data that fits in memory

---

**The key line that does everything:**
```c
cmox_hash_compute(CMOX_SHA256_ALGO, message, sizeof(message), hash, 32, &hash_size)
```

**What each parameter does:**

1. **`CMOX_SHA256_ALGO`** - Which algorithm to use
   - Tells the function: "Use SHA256 algorithm"
   - Other options: `CMOX_SHA1_ALGO`, `CMOX_SHA384_ALGO`, etc.
   - **How it works**: Selects the mathematical algorithm for hashing

2. **`message`** - Pointer to your data
   - Points to the bytes you want to hash
   - Can be text, binary data, file contents, anything
   - **Example**: `"Hello World"` or an array of bytes

3. **`sizeof(message)`** - Size in bytes of your data
   - Tells the function exactly how many bytes to process
   - **Critical**: If wrong, hash will be incorrect
   - **Example**: 11 for "Hello World", 256 for a 256-byte file

4. **`hash`** - Where to store the result
   - Pointer to a buffer that will receive the 32-byte hash
   - Must be at least 32 bytes in size
   - After function returns, contains the computed SHA256 hash
   - **Example**: `uint8_t hash[32];`

5. **`32`** - Expected output size
   - SHA256 always produces exactly 32 bytes
   - This parameter verifies the output matches expectations
   - Never change this for SHA256

6. **`&hash_size`** - Receives the actual size written
   - Function writes back how many bytes it actually computed
   - Should always be 32 for SHA256
   - Use this to verify: `if (hash_size != 32) /* error! */`

---

**How to use it:**

```c
uint8_t message[] = "Hello, STM32!";
uint8_t hash[32];
size_t hash_size;

cmox_hash_compute(CMOX_SHA256_ALGO, message, strlen((char*)message), hash, 32, &hash_size);
```

---

**Return codes (what the function tells you):**

| Return Code | Meaning | What to do |
|---|---|---|
| `CMOX_HASH_SUCCESS` | Worked! Hash is computed | Check the `hash` buffer - it's ready to use |
| `CMOX_HASH_ERR_PARAMETER` | Invalid pointer or size | Verify all pointers are valid and sizes make sense |
| `CMOX_HASH_ERR_BAD_OPERATION` | Wrong function sequence | Ensure you initialized the library first |

**Check the result:**
```c
if (cmox_hash_compute(...) != CMOX_HASH_SUCCESS)
    Error_Handler();  /* Hash computation failed */
```

---

**Real-world example:**
```c
/* Step 1: Prepare your data */
uint8_t myData[] = {0x01, 0x02, 0x03, 0x04, 0x05};

/* Step 2: Create buffer for hash result */
uint8_t myHash[32];
size_t myHashSize;

/* Step 3: Hash it in one call */
cmox_hash_compute(CMOX_SHA256_ALGO, myData, 5, myHash, 32, &myHashSize);

/* Step 4: Now myHash contains your SHA256 digest (32 bytes) */
```

---

### Method 2: Multiple Calls (Chunked Processing)

**Best for**: Large files, streaming data, or when you can't load all data into memory at once

---

**Step 1: Create a SHA256 context**
```c
cmox_sha256_handle_t sha256_ctx;
cmox_hash_handle_t *hash_ctx = cmox_sha256_construct(&sha256_ctx);
```
- **What it does**: Creates an empty "container" to hold SHA256 state
- **Why needed**: Lets you process data piece by piece instead of all at once
- **How it works**: Allocates internal memory to track hashing progress
- **Returns**: Pointer to the context (use this in all following calls)
- **Check result**: `if (hash_ctx == NULL)` means it failed

---

**Step 2: Initialize the context**
```c
cmox_hash_init(hash_ctx)
```
- **What it does**: Prepares the context for hashing (sets initial values)
- **Why needed**: Without this, the context is empty and won't work
- **How it works**: Sets up SHA256 internal state machine to "ready"
- **Must do**: Call this right after creating the context
- **Return check**: `!= CMOX_HASH_SUCCESS` means initialization failed

---

**Step 3: Append chunks of data (call this multiple times)**
```c
cmox_hash_append(hash_ctx, &data_chunk[0], chunk_size)
```
- **What it does**: Feeds more data into the hash computation
- **`hash_ctx`** - The context you created (holds the state)
- **`&data_chunk[0]`** - Pointer to bytes to add (can be small or large)
- **`chunk_size`** - How many bytes to add this time
- **How it works**: Updates internal hash state with new data (like mixing more ingredients)
- **Call it**: As many times as you have data chunks
- **Example**: 
  ```c
  cmox_hash_append(hash_ctx, chunk1, 100);  /* Add first 100 bytes */
  cmox_hash_append(hash_ctx, chunk2, 50);   /* Add next 50 bytes */
  cmox_hash_append(hash_ctx, chunk3, 25);   /* Add final 25 bytes */
  ```

---

**Step 4: Generate the final hash**
```c
cmox_hash_generateTag(hash_ctx, final_hash, &final_size)
```
- **What it does**: Finishes hashing and outputs the computed hash
- **`hash_ctx`** - Your context with all appended data
- **`final_hash`** - Buffer to receive the 32-byte hash
- **`&final_size`** - Receives actual size (should be 32)
- **How it works**: Performs final calculations and writes result to your buffer
- **Important**: Call this AFTER you've appended all your data
- **One-time only**: Don't call `cmox_hash_append()` after this

---

**Step 5: Clean up the context**
```c
cmox_hash_cleanup(hash_ctx)
```
- **What it does**: Frees memory used by the context
- **Why needed**: Prevents memory leaks
- **When to call**: After you got your hash from `generateTag()`
- **Can reuse**: After cleanup, you can construct a new context for another hash

---

**Complete example with all 5 steps:**
```c
/* 1. Create context */
cmox_sha256_handle_t sha256_ctx;
cmox_hash_handle_t *hash_ctx = cmox_sha256_construct(&sha256_ctx);

/* 2. Initialize */
cmox_hash_init(hash_ctx);

/* 3. Append data chunks (multiple times) */
cmox_hash_append(hash_ctx, chunk1, 100);
cmox_hash_append(hash_ctx, chunk2, 100);
cmox_hash_append(hash_ctx, chunk3, 56);

/* 4. Get final hash */
uint8_t final_hash[32];
size_t final_size;
cmox_hash_generateTag(hash_ctx, final_hash, &final_size);

/* 5. Clean up */
cmox_hash_cleanup(hash_ctx);

/* Now final_hash contains your SHA256 digest */
```

---

**Why use this method?**
- Process 1MB file without loading all 1MB into RAM
- Hash data as it arrives from network/sensor
- Hash data larger than available memory

---

## Complete Code Example

Here's how to use both methods in practice, with focus on the key lines:

### Starting Up

**Line that initializes everything:**
```c
cmox_initialize(&init_target)
```
- Do this once at program start
- Without it, all crypto functions fail
- Put this after `HAL_Init()` and system clock setup

---

### Quick Hash (Single Call)

**This one line hashes your entire message:**
```c
cmox_hash_compute(CMOX_SHA256_ALGO, message, strlen((char*)message), computed_hash, 32, &computed_size)
```
- **Input**: Your message data
- **Output**: `computed_hash` buffer filled with 32-byte SHA256
- **Use when**: Data is small enough to fit in memory
- **Speed**: Very fast for small messages

---

### Large Data Hash (Multiple Calls)

**Create once:**
```c
cmox_hash_handle_t *hash_ctx = cmox_sha256_construct(&sha256_ctx)
```

**Loop through your data chunks:**
```c
for (int i = 0; i < total_size; i += CHUNK_SIZE)
    cmox_hash_append(hash_ctx, &data[i], CHUNK_SIZE)
```
- **Feeds data piece by piece**
- **CHUNK_SIZE** can be 48, 256, 1024 - whatever fits your RAM
- **Flexible**: Add one byte or 10KB, doesn't matter

**Then finalize:**
```c
cmox_hash_generateTag(hash_ctx, computed_hash, &computed_size)
```
- Gets the final 32-byte hash
- After this, don't call append() anymore

---

### Shutting Down

**This line cleans up:**
```c
cmox_finalize(NULL)
```
- Do this once at program end
- Frees all crypto resources
- Called only once, after all hashing is done

---

## Error Handling

### Check Every Operation

**The line that matters:**
```c
if (cmox_hash_compute(...) != CMOX_HASH_SUCCESS)
```
- **What it does**: Verifies the operation worked
- **Why needed**: Function might fail (bad input, memory issue, etc.)
- **Always check**: Every crypto function returns a status code

---

### Return Codes Explained

**Success case:**
```c
if (retval == CMOX_HASH_SUCCESS)  /* Worked! Hash is ready */
```

**Parameter error:**
```c
if (retval == CMOX_HASH_ERR_PARAMETER)  /* Bad pointer or size */
```
- Fix: Check that your pointers are valid and sizes make sense

**Operation error:**
```c
if (retval == CMOX_HASH_ERR_BAD_OPERATION)  /* Wrong sequence */
```
- Fix: Make sure you called init before append, etc.

---

### Verify Hash Output

**Check the computed size:**
```c
if (computed_size != CMOX_SHA256_SIZE)  /* Should always be 32 */
```
- **What it does**: Verifies the library computed full hash
- **Expected value**: Always 32 for SHA256
- **If wrong**: Something went wrong in computation

---

### Compare Hashes

**The comparison line:**
```c
if (memcmp(expected_hash, computed_hash, 32) == 0)
```
- **What it does**: Byte-by-byte comparison of two hashes
- **Returns**: 0 if identical, non-zero if different
- **Use case**: Verify received hash matches computed hash
- **Why it matters**: Detects if data was tampered with

---

## Tips & Best Practices

### ✅ Do's

**1. Always initialize first**
```c
cmox_initialize(&init_target)
```
- Put this at program start, after system setup
- Without it, everything else fails

**2. Always finalize last**
```c
cmox_finalize(NULL)
```
- Put this at program end
- Cleans up memory and resources

**3. Check return values**
```c
if (cmox_hash_compute(...) != CMOX_HASH_SUCCESS)
```
- Every. Single. Function.
- Don't assume it worked

**4. Use the right method**
```c
cmox_hash_compute(...)  /* For small data, quick & simple */
cmox_hash_append(...)   /* For large data, piece by piece */
```
- Single call = easier, but needs all data in RAM
- Multiple calls = harder, but handles huge files

**5. Verify results**
```c
if (memcmp(expected, computed, 32) == 0)  /* Hashes match */
```
- Always compare hashes to detect tampering

---

### ❌ Don'ts

**1. Don't skip initialization**
```c
/* WRONG - will crash */
cmox_hash_compute(...)
cmox_finalize(...)

/* RIGHT */
cmox_initialize(...)
cmox_hash_compute(...)
cmox_finalize(...)
```

**2. Don't hardcode buffer sizes**
```c
/* WRONG */
uint8_t hash[32];  /* What if SHA384 needs 48? */

/* RIGHT */
uint8_t hash[CMOX_SHA256_SIZE];  /* Use the constant */
```

**3. Don't reuse context without cleanup**
```c
/* WRONG - reusing without cleanup */
cmox_hash_construct(&ctx)
cmox_hash_init(&ctx)
cmox_hash_append(...)
/* Now trying to use same ctx again... */

/* RIGHT */
cmox_hash_construct(&ctx)
cmox_hash_init(&ctx)
cmox_hash_append(...)
cmox_hash_cleanup(&ctx)  /* Clean before reuse */
```

**4. Don't forget headers**
```c
/* WRONG - missing include */
#include <string.h>
/* memcmp not available! */

/* RIGHT */
#include "cmox_crypto.h"
#include <string.h>  /* Needed for memcmp */
```

**5. Don't ignore error codes**
```c
/* WRONG - ignoring failure */
cmox_hash_compute(CMOX_SHA256_ALGO, msg, 100, hash, 32, &size);
/* Hash might be garbage, but we use it anyway */

/* RIGHT */
if (cmox_hash_compute(...) != CMOX_HASH_SUCCESS)
    return;  /* Don't use invalid hash */
```

---

## Quick Reference

### The 7 Essential Functions

**1. Initialize (do once at start)**
```c
cmox_initialize(&init_target)
```
- Sets up the entire cryptographic library
- Must be first before any crypto operation

**2. Single-call hash (for small data)**
```c
cmox_hash_compute(CMOX_SHA256_ALGO, message, msg_size, hash, 32, &hash_size)
```
- Does everything in one line
- Input: your data | Output: hash buffer

**3. Create context (for chunked hashing)**
```c
cmox_sha256_construct(&sha256_ctx)
```
- Creates an empty container for hash state
- Needed when processing large data piece by piece

**4. Initialize context (after creating)**
```c
cmox_hash_init(hash_ctx)
```
- Prepares the context for hashing
- Must be called before appending data

**5. Add data chunks (call many times)**
```c
cmox_hash_append(hash_ctx, data_chunk, chunk_size)
```
- Feeds more data into the hash
- Call once per chunk

**6. Get final hash (after all appends)**
```c
cmox_hash_generateTag(hash_ctx, hash_buffer, &hash_size)
```
- Computes the final 32-byte hash
- Call only once, after all appends complete

**7. Cleanup context (before reusing)**
```c
cmox_hash_cleanup(hash_ctx)
```
- Frees the context memory
- Needed before creating a new context

**8. Finalize (do once at end)**
```c
cmox_finalize(NULL)
```
- Shuts down entire cryptographic library
- Call only once, at very end of program

---

### Constants You'll Use

```c
CMOX_SHA256_SIZE        /* Always 32 for SHA256 */
CMOX_SHA256_ALGO        /* Tells library to use SHA256 */
CMOX_HASH_SUCCESS       /* Operation succeeded */
CMOX_HASH_ERR_PARAMETER /* Bad input parameters */
CMOX_HASH_ERR_BAD_OPERATION  /* Wrong function sequence */
```

---

## Example: Hash Verification (Practical Use Case)

**Scenario**: You receive a message + hash. Verify the message hasn't been tampered with.

**The key lines:**

**Step 1: Compute hash of received message**
```c
cmox_hash_compute(CMOX_SHA256_ALGO, received_message, msg_length, computed_hash, 32, &size)
```
- Takes your received data and hashes it

**Step 2: Compare with expected hash**
```c
if (memcmp(expected_hash, computed_hash, 32) == 0)
```
- Compares byte-by-byte
- If identical → message is authentic
- If different → message was modified

**Real example:**
```c
/* You received these */
uint8_t message[] = "Secret data from server";
uint8_t expected_hash[] = {0x46, 0x50, 0x0b, 0x6a, ...};  /* 32 bytes */

/* You compute */
uint8_t computed_hash[32];
size_t size;
cmox_hash_compute(CMOX_SHA256_ALGO, message, strlen((char*)message), 
                  computed_hash, 32, &size);

/* You check */
if (memcmp(expected_hash, computed_hash, 32) == 0)
    printf("✓ Message is authentic and unchanged\n");
else
    printf("✗ Message has been tampered with!\n");
```

**Why this works:**
- Even 1 bit change in message → completely different hash
- Impossible to forge hash without knowing algorithm (SHA256)
- Simple way to detect tampering

---

## Conclusion

The STM Cryptographic Library makes it easy to add cryptographic security to STM32 applications. Start with **Method 1 (Single Call)** for simplicity, then move to **Method 2 (Multiple Calls)** when you need to handle larger data or streaming scenarios.

**Key takeaways**:
- Initialize and finalize the library properly
- Check all return codes
- Use the appropriate method for your use case
- Always verify hash results when needed

Happy hashing! 🔒
