# Data Parsing & Logic — `code/parse`

This subsystem provides the engine's data ingestion layer, ranging from low-level tokenization of "Table" files (.tbl) to the evaluation of complex, event-driven mission logic via S-Expressions (SEXP).

## Architecture

The parsing subsystem is divided into two primary layers: raw text tokenization and logical tree evaluation.

### 1. Low-Level Parser (`parselo.h`)
The foundation for all data reading in the engine. It operates on a global `Parse_text` buffer.
*   **Tokenization:** Functions like `required_string()`, `optional_string()`, and `stuff_int()` move a global pointer (`Mp`) through the text.
*   **Modular Tables:** `parse_modular_table()` enables the engine's "Table Extension" (.tbm) system, allowing mods to append or override base data without modifying core files.
*   **String Utilities:** Comprehensive handling of white/gray space, line endings, and case-insensitive lookups.

### 2. High-Level Parser (`parsehi.h`)
Standardizes the extraction of complex engine types:
*   Standardized patterns for optional fields (`parse_optional_bool_into`).
*   Color parsing for `hdr_color`.

### 3. S-Expressions (SEXP) (`sexp.h`)
The "scripting" language of FreeSpace 2. It defines the logic for mission goals, events, and AI triggers.
*   **Structure:** Uses a node-based system inspired by LISP. Each node has a `first` (CAR) and `rest` (CDR) index.
*   **Operators:** Hundreds of hardcoded operators (e.g., `OP_WHEN`, `OP_HAS_ARRIVED_DELAY`) that the engine evaluates every frame.
*   **Variables:** `Sexp_variables` allow missions to track state (numbers or strings) that can persist across mission loops.
*   **Containers:** Modern extensions (`sexp_container.h`) add support for lists and maps within the SEXP system.

### 4. Utilities
*   **`md5_hash.h`**: Used for data integrity checks and cache validation.
*   **`encrypt.h`**: Handles encryption for sensitive data (e.g., pilot files, network packets).
*   **`generic_log.h`**: A base class for subsystem-specific logging (e.g., `mission.log`).

## Design Patterns

### The "Stuff" Pattern
Most parsing functions use a "stuff" nomenclature (e.g., `stuff_int`, `stuff_vec3d`). This means the function finds the next relevant token and "stuffs" its converted value into the provided pointer.

### Failure Policy
The parser is "loud" in debug builds but permissive in release:
*   `required_string()` will trigger an `Error()` (crash) if a token is missing in debug mode, ensuring table errors are caught during development.

## Interaction with Vulkan

The parsing subsystem indirectly supports Vulkan by:
*   **Material Definitions:** Reading shader flags, blend modes, and texture assignments from `model_textures.tbl` and ship tables.
*   **Shader Variants:** The SEXP system can trigger graphical changes (e.g., `OP_SHIP_EFFECT`, `OP_DEACTIVATE_GLOW_MAPS`) that the Vulkan backend must interpret and apply to the command stream.

## Documentation Note: SEXP Wiki
The S-Expression system is too large for a single README. The `Operators` list contains over 400 unique logic triggers. 
*   **Wiki Requirement:** A dedicated technical wiki is recommended for documenting individual SEXP operators, their argument types (`OPF_*`), and evaluation contexts.

## Common Pitfalls
*   **Global Pointer Management:** `Mp` is a global. If a function returns without correctly advancing or resetting `Mp` after a nested parse, the entire table loading sequence will fail.
*   **Buffer Limits:** `PARSE_TEXT_SIZE` is a hard limit (1MB). Extremely large mod tables can exceed this, requiring modularization into `.tbm` files.
*   **Token Found Flag:** `Token_found_flag` must be manually cleared/checked when doing complex conditional parsing.
