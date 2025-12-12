# Opening Books

This directory contains Polyglot (.bin) opening books for use with the c3 chess engine.

## Available Books

| Book | Entries | Size | Description |
|------|---------|------|-------------|
| `gm2001.bin` | 30,416 | 475 KB | Grandmaster games collection |
| `komodo.bin` | 578,126 | 8.8 MB | Komodo chess engine book (comprehensive) |
| `rodent.bin` | 175,355 | 2.7 MB | Rodent chess engine book |

## Usage

```
./c3
setoption name OwnBook value true
setoption name BookFile value books/komodo.bin
isready
```

## Attribution

These opening books are sourced from:

**[gmcheems-org/free-opening-books](https://github.com/gmcheems-org/free-opening-books)**

Thank you to the maintainers for making these books freely available.

## Recommendations

- **For general play**: Use `komodo.bin` (most comprehensive)
- **For faster loading**: Use `gm2001.bin` (smaller, loads quickly)
- **For variety**: Use `rodent.bin` (good balance of size and coverage)

## License

Please refer to the original repository for licensing information:
https://github.com/gmcheems-org/free-opening-books
