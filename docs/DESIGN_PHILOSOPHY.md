# Type-Driven Design: A Philosophy

## The Core Idea

**Use the type system to make wrong code unwritable.** Don't rely on programmer discipline, runtime checks, or documentation. If invalid code compiles, the types are wrong.

This isn't about elegance. It's about making entire categories of bugs structurally impossible.

---

## Typestate: The ShuffledDeck Pattern

You're coding a card game. Games should only start with a shuffled deck.

**Common approach:**

```cpp
struct Deck { ... };
Deck shuffle(Deck d);
void startGame(Deck d);  // hopes caller shuffled
```

Programmer must remember to shuffle. Nothing prevents `startGame(unshuffledDeck)`. You'll add a comment. Someone will ignore it. You'll add a runtime check. It'll fire in production.

**Type-driven approach:**

```cpp
class Deck { ... };

class ShuffledDeck {
    friend ShuffledDeck shuffle(Deck&&);
    ShuffledDeck() = default;  // private
};

[[nodiscard]] ShuffledDeck shuffle(Deck&& d);  // consumes Deck, returns ShuffledDeck
void startGame(ShuffledDeck d);                // won't compile with Deck
```

The `shuffle` function is a **typestate transition** — it consumes the precursor state and produces the valid state. Move semantics (approximating affine types) prevent reusing the unshuffled deck.

Successful compilation is proof that no one can sneak in a bad deck.

---

## Parametricity

Consider a function that returns an element from a list:

**Non-polymorphic:**

```cpp
Card get_first(const std::vector<Card>& list);
```

Inside this function, I can inspect the cards, change behavior based on suit, log values. Caller has no guarantees without reading the implementation.

**Polymorphic:**

```cpp
template <typename T>
T get_first(const std::vector<T>& list);
```

Because the function is generic over `T`, it *approximates* parametricity — it cannot observe or modify `T` in type-specific ways. The signature `forall A. [A] -> A` constrains the implementation to pure data movement.

C++ leaks here (templates can specialize, `if constexpr` exists, `type_traits` allow inspection). But the constraint makes the correct implementation obvious and type-specific tricks awkward.

---

## The Shared Foundation

| Pattern                                 | What It Eliminates    |
| --------------------------------------- | --------------------- |
| Typestate (`Deck` → `ShuffledDeck`)     | Invalid sequencing    |
| Parametricity (`template <typename T>`) | Invalid operations    |
| Move semantics                          | Use-after-transition  |
| State as location                       | State enum mismatches |
| Capability tokens                       | Phase violations      |

All are variations of one idea: encode invariants in types so the compiler enforces them.

---

## Ownership

**Make coordination unnecessary by making ownership complete.** Objects own everything they need to act. If two pieces of code must agree about state, one is wrong about what it owns.

```cpp
// WRONG: Fragmented ownership
class Uploader {
    bool upload(TextureId id, GpuTexture* out);  // caller owns output, checks success
};

// RIGHT: Complete ownership
class Uploader {
    GpuTexture upload(TextureId id);  // returns owned object or doesn't return
};
```

**Corollary:** Retry loops prove the architecture is broken. If you retry, two components disagreed about reality. Don't retry — fix the ownership so they can't disagree.

---

## State as Location

**Container membership is state.** Objects don't carry state enums or status flags. An object's state is determined by which container holds it.

```cpp
// WRONG: State as data
struct TextureRecord {
    enum State { Missing, Queued, Resident, Failed } state;
    std::optional<GpuTexture> gpu;  // valid only if state == Resident (trust me)
};
std::map<TextureId, TextureRecord> m_textures;

// RIGHT: State as location
struct UploadRequest { std::string path; };
struct GpuTexture { Handle handle; };

std::map<TextureId, UploadRequest> m_pending;   // presence = queued
std::map<TextureId, GpuTexture> m_resident;     // presence = resident
// absent from both = missing
```

No state enum to get out of sync. No `std::optional` that might lie. Iterating `m_resident` guarantees valid handles. The data structure *is* the state machine.

---

## Capability Tokens

**Capability tokens prove phase validity.** If an operation is only valid during a specific phase, require a capability token that can only exist during that phase.

```cpp
// Only constructible by Renderer
class UploadPhase {
    friend class Renderer;
    UploadPhase() = default;
public:
    UploadPhase(const UploadPhase&) = delete;
};

void submitToGpu(const UploadPhase& proof, GpuHandle h);
```

No `assert(isUploadPhaseActive)` required. The code cannot execute without the token. The compiler verifies this.

This is the ShuffledDeck pattern applied to temporal phases. If you have an `UploadPhase` token, you're in the upload phase. No token, no call.

---

## Boundaries vs Internals

**Conditionals at system boundaries are acceptable.** That's where you reject invalid input before it enters the system.

```cpp
class ConnectedSocket {
    ConnectedSocket(int fd);  // private
public:
    static std::optional<ConnectedSocket> connect(IpAddr addr);  // boundary
    void send(Data d);  // internal: cannot fail due to connection state
};
```

Caller checks the optional *once*, at the boundary. Inside the system, `ConnectedSocket` exists means connected. No further checks.

**Domain-optional is real absence; state-optional is fake absence caused by a leaky internal state model.**

**Conditionals inside the system to route around state that shouldn't exist — that's the disease.** Guards don't protect the system from invalid state. Guards protect invalid state from being noticed.

---

## The Death List

**Inhabitant branching:** branching on whether a value exists (`nullptr`, empty `optional`) inside internal logic instead of making existence the proof by construction.

When reviewing code, delete:

| Pattern                                                                                          | Problem                                                   |
| ------------------------------------------------------------------------------------------------ | --------------------------------------------------------- |
| `enum State { Init, Ready, Error }`                                                              | State as data, not location                               |
| `if (ptr != nullptr)` deep in logic                                                              | Inhabitant branching (move to boundary)                   |
| `if (opt.has_value())` deep in logic                                                             | Inhabitant branching (move to boundary)                   |
| `switch (state)` / `std::visit` used to route around representational state that shouldn't exist | Control flow over invalid state instead of eliminating it |
| `bool isInitialized`, `bool hasData`                                                             | Object existence *is* the proof                           |
| Sentinel values (`-1`, `UINT32_MAX`)                                                             | Fake inhabitants in the domain                            |
| `void init(); void update();`                                                                    | Step-coupling (constructor must complete init)            |
| Retry loops                                                                                      | Two components disagreed — fix ownership                  |
| Guards / asserts for "impossible" states                                                         | Protecting invalid state from being noticed               |
| `Failed` enum variants                                                                           | Failure is absence, not presence                          |

---

## Approximations in C++

C++ lacks dependent types, has weak parametricity, and permits escape hatches (`const_cast`, `reinterpret_cast`, global state). These patterns approximate the ideal:

| Ideal                               | C++ Approximation                                       |
| ----------------------------------- | ------------------------------------------------------- |
| Typestate (`Deck` → `ShuffledDeck`) | Move-only types, private constructors, friend factories |
| Linear types                        | `= delete` copy, move-only, `[[nodiscard]]`             |
| Parametricity                       | Templates with minimal constraints                      |
| Capability tokens                   | Structs with private constructors, `friend` access      |
| State as location                   | Separate containers per state                           |

The approximations leak. Someone can `std::move` from the same object twice. But the types make the correct path obvious and the incorrect path awkward. That's usually enough.

---

## The Test

When reviewing a design, don't ask:

> "What happens if someone passes an unshuffled deck?"

Ask:

> "How do I make passing an unshuffled deck a compile error?"

If the answer involves runtime checks, guards, asserts, or documentation — the types are wrong. Redesign until the compiler is the enforcer.

---

## Not Acceptable

The following are not valid objections:

* "Polymorphism is overkill for this"
* "It's just a boolean"
* "Simpler fix" that moves the branch instead of eliminating the invalid state
* "Performance concerns" (vtables, allocations, container overhead)
* "The language doesn't support it perfectly"

"Simpler" is not the goal. Correct-by-construction is the goal. The runtime cost of a container lookup is infinitely cheaper than debugging a state mismatch that only manifests in production.

---

The goal is a codebase where "it compiles" means "it's not wrong in any of the ways the type system can catch." Push that boundary as far as the language allows.
