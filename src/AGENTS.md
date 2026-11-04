# SRC/ KNOWLEDGE BASE

## OVERVIEW

Core C++ source for DigiByte. Bitcoin Core v26.2 base with multi-algo mining, Dandelion++, and DigiDollar.

For all work, follow the
[lean documentation rule](../doc/design/paymaster-integration-boundaries.md#lean-documentation).

## PAYMASTER BOUNDARIES

Follow [the canonical integration rules](../doc/design/paymaster-integration-boundaries.md)
for `paymaster/`, `wallet/paymaster*`, `wallet/rpc/paymaster*`, `rpc/paymaster*`,
`qt/paymaster*` and their shared-Core callers. Consult `PAYMASTER.md` and the
Paymaster entries in `REPO_MAP_DIGIDOLLAR.md` before choosing a component.
Keep domain rules shared, adapters thin and derived indexes non-authoritative.
Document each shared-Core integration and its ordinary-wallet regression checks.
Do not expand scope or weaken validation/tests/policy to complete a refactor.
The versioned boundary policy and lint checks implement the mechanical subset
of these rules; passing them does not replace semantic review.

## STRUCTURE

```
src/
├── consensus/      # Consensus rules, DCA, DigiDollar validation (AGENTS.md)
├── crypto/         # Multi-algorithm hashing (AGENTS.md)
├── digidollar/     # DigiDollar stablecoin implementation
├── oracle/         # Price oracle for DigiDollar
├── wallet/         # Wallet with DGB address formats
│   └── rpc/        # Wallet RPC commands
├── rpc/            # Node RPC (getblockreward, getmininginfo)
├── qt/             # Qt5 GUI
├── node/           # Node management, validation
├── policy/         # Fee estimation, mempool policy
├── script/         # Script interpreter
├── net*.cpp/h      # P2P networking
├── dandelion.cpp   # Dandelion++ privacy
├── stempool.h      # Dandelion stempool
├── pow.cpp/h       # Multi-algorithm PoW
└── validation.cpp  # Block/tx validation
```

## WHERE TO LOOK

| Task | Files | Notes |
|------|-------|-------|
| Multi-algo PoW | `pow.cpp`, `pow.h` | GetNextWorkRequired per algo |
| Block version | `primitives/block.h` | BLOCK_VERSION_ALGO mask |
| Difficulty | `consensus/dca.cpp` | DigiShield, MultiShield |
| DigiDollar tx | `digidollar/*.cpp` | txbuilder, validation |
| Oracle prices | `oracle/` | External price feeds |
| Dandelion++ | `dandelion.cpp`, `stempool.h` | Two-pool privacy |
| Address encode | `base58.cpp`, `bech32.cpp` | DGB prefixes |
| Chain params | `kernel/chainparams.cpp` | Network constants |
| Custom RPC | `rpc/mining.cpp` | getblockreward, getdifficulty |

## DIGIBYTE-SPECIFIC FILES

**Not in Bitcoin Core:**
- `pow.cpp` - Multi-algorithm difficulty selection
- `dandelion.cpp`, `stempool.h` - Dandelion++ privacy
- `digidollar/` - Entire directory (stablecoin)
- `oracle/` - Price oracle integration
- `consensus/dca.cpp` - DigiByte difficulty adjustment
- `consensus/volatility.cpp` - DigiDollar volatility tracking
- `crypto/hash*.h` - Groestl, Skein, Qubit, Odocrypt

## KEY MODIFICATIONS TO BITCOIN

| File | Change |
|------|--------|
| `chainparams.cpp` | DGB ports, prefixes, fork heights |
| `amount.h` | 21B max supply |
| `validation.cpp` | Multi-algo block validation |
| `net.cpp` | Dandelion integration |
| `txmempool.cpp` | Stempool coordination |

## NAMING CONVENTIONS

```cpp
// Variables
m_member_variable      // Class members
g_global_variable      // Globals
local_variable         // Locals

// Classes
class ClassName        // PascalCase, no C prefix
struct DataStruct

// Constants
CONSTANT_NAME          // ALL_CAPS
```

## BUILD TARGETS

| Binary | Main Entry | Purpose |
|--------|------------|---------|
| digibyted | `digibyted.cpp` | Daemon |
| digibyte-qt | `qt/main.cpp` | GUI wallet |
| digibyte-cli | `digibyte-cli.cpp` | RPC client |
| digibyte-tx | `digibyte-tx.cpp` | TX tool |
| digibyte-wallet | `digibyte-wallet.cpp` | Wallet tool |

## LIBRARY DEPENDENCIES

```
libdigibyte_consensus  # Consensus-critical code
libdigibyte_common     # Shared utilities
libdigibyte_node       # Node-specific code
libdigibyte_wallet     # Wallet code
libsecp256k1           # Elliptic curve crypto
libleveldb             # Database
libunivalue            # JSON parsing
```
