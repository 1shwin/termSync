# TermSync - A Terminal Text Editor for Real-Time Collaboration

A feature-rich, real-time collaborative text editor written in C with terminal UI support. Multiple users can simultaneously edit the same document with live cursor tracking, text selection, rich formatting, and intelligent conflict resolution.

![Version](https://img.shields.io/badge/version-1.0.0-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![Language](https://img.shields.io/badge/language-C11-orange)

## ✨ Features

### Real-Time Collaboration
- **Multi-User Editing**: Support for up to 10 simultaneous editors
- **Live Cursors**: See other users' cursor positions in real-time with color-coded indicators
- **Live Selections**: View other users' text selections with colored highlights
- **Automatic Synchronization**: All changes propagate instantly to all connected clients

### Rich Text Formatting
- **Bold Text**: Apply bold formatting with `Ctrl+B`
- **Underline Text**: Apply underline formatting with `Ctrl+U`
- **Selection Formatting**: Format entire text selections at once
- **Per-Character Formatting**: Mix bold and underlined text freely

### Text Selection & Editing
- **Keyboard Selection**: Use `Shift+Left/Right` arrow keys to select text
- **Visual Feedback**: Selected text appears with reverse video highlighting
- **Selection Operations**: Delete, format, or overwrite selections with a single keystroke
- **Multi-Line Selection**: Select across multiple lines seamlessly

### Code Execution
- **Inline Code Blocks**: Write C code within `` ``` `` code fences
- **Remote Execution**: Run code blocks with `Ctrl+R`
- **Compile & Execute**: Automatic compilation with GCC and execution
- **Output Display**: Results appear in the status bar

### Advanced Conflict Resolution
- **Disconnect Handling**: Continue editing while offline
- **Offline Queue**: Stores up to 100 operations made while disconnected
- **Automatic Reconnection**: Seamlessly reconnect and sync when network restores
- **Smart Sync Strategies**:
  - **Incremental Sync**: Receive only missed operations if possible
  - **Full Resync**: Complete document refresh for major divergence
  - **Replay Mechanism**: Your offline edits are replayed on top of server state
- **Version Tracking**: Document versioning prevents data loss
- **Hash Verification**: Cryptographic hashing detects inconsistencies

### User Interface
- **Terminal-Based**: Runs in any terminal with ncurses support
- **Color-Coded Users**: Each user gets a unique color (up to 7 colors)
- **Status Bar**: Shows line/column, formatting state, selection count, connection status, and version
- **Real-Time Feedback**: Instant visual updates for all operations
- **Connection Indicator**: Always know if you're connected, syncing, or offline

## 🔧 Dependencies

### Required
- **GCC**: C compiler (version 4.9 or later)
- **ncurses**: Terminal UI library
- **POSIX Threads**: pthread library (usually built-in on Unix systems)
- **Standard C Library**: C11 standard

### Optional
- **GCC** (for code execution feature): Must be available in PATH

## 📦 Installation

### Ubuntu/Debian
```bash
# Install dependencies
sudo apt-get update
sudo apt-get install build-essential libncurses5-dev libncursesw5-dev

# Clone or download the repository
git clone https://github.com/1shwin/mmga-megathon25.git
cd mmga-megathon25

# Compile using the makefile
make
```

### Fedora/RHEL/CentOS
```bash
# Install dependencies
sudo dnf install gcc ncurses-devel

# Clone or download the repository
git clone https://github.com/1shwin/mmga-megathon25.git
cd mmga-megathon25

# Compile using the makefile
make
```

### macOS
```bash
# Install Xcode Command Line Tools (includes GCC)
xcode-select --install

# ncurses is pre-installed on macOS

# Clone or download the repository
git clone https://github.com/1shwin/mmga-megathon25.git
cd mmga-megathon25

# Compile using the makefile
make
```

### Arch Linux
```bash
# Install dependencies
sudo pacman -S base-devel ncurses

# Clone or download the repository
git clone https://github.com/1shwin/mmga-megathon25.git
cd mmga-megathon25

# Compile using the makefile
make```

## 🚀 Usage

### Starting the Server

```bash
./server
```

The server will output:
```
--- C Collaborative Editor Server ---
WARNING: Code execution is ENABLED. Run only on a trusted LAN.
Listening on port 8080
Server IP address: 192.168.1.100
```

**Note the IP address** - clients will need this to connect.

### Starting a Client

```bash
./client <server_ip>
```

Example:
```bash
./client 192.168.1.100
# Or for local testing:
./client 127.0.0.1
```

### Multiple Clients

Open multiple terminals and start additional clients:
```bash
# Terminal 1
./client 192.168.1.100

# Terminal 2
./client 192.168.1.100

# Terminal 3
./client 192.168.1.100
```

All clients will see each other's edits, cursors, and selections in real-time.

## ⌨️ Keyboard Shortcuts

### Navigation
| Key | Action |
|-----|--------|
| `Arrow Keys` | Move cursor and clear selection |
| `Shift+Left` | Extend selection left |
| `Shift+Right` | Extend selection right |

### Editing
| Key | Action |
|-----|--------|
| `Backspace` | Delete character before cursor (or delete selection) |
| `Delete` | Delete character after cursor |
| `Enter` | Insert newline |
| `Printable Characters` | Insert character (or replace selection) |

### Formatting
| Key | Action |
|-----|--------|
| `Ctrl+B` | Toggle bold (on selection or next character) |
| `Ctrl+U` | Toggle underline (on selection or next character) |

### Advanced
| Key | Action |
|-----|--------|
| `Ctrl+R` | Run code block (finds first `` ``` ... ``` `` block) |
| `F1` | Exit editor |

### Status Bar Information
The bottom status bar displays:
- **Line & Column**: Current cursor position (e.g., `Ln 5, Col 12`)
- **Format**: Active formatting (`B` = Bold, `U` = Underline)
- **Selection**: Number of selected characters (e.g., `Sel: 23`)
- **Connection Status**: `[Connected]`, `[Syncing...]`, or `[OFFLINE]`
- **Version**: Document version number (e.g., `v142`)

## 📝 Example Workflow

### Basic Collaborative Editing

1. **Start Server**
   ```bash
   ./server
   ```

2. **Connect Two Clients**
   ```bash
   # Terminal 1
   ./client 192.168.1.100
   
   # Terminal 2
   ./client 192.168.1.100
   ```

3. **Type in Any Client**
   - Both clients instantly see the text
   - Cursors are color-coded
   - Each user sees the other's cursor position

4. **Format Text**
   - Select text with `Shift+Arrow` keys
   - Press `Ctrl+B` for bold or `Ctrl+U` for underline
   - Formatting applies across all clients

### Code Execution Example

1. **Type a Code Block**
   ```
   ```
   #include <stdio.h>
   int main() {
       printf("Hello from collaborative editor!\n");
       return 0;
   }
   ```
   ```

2. **Press `Ctrl+R`**
   - Code compiles and runs on the server
   - Output appears in status bar: `Hello from collaborative editor!`

### Handling Disconnections

1. **Work While Connected**
   - Edit normally
   - Status shows `[Connected] v50`

2. **Disconnect** (simulate by stopping server or network)
   - Status changes to `[OFFLINE] v50`
   - Continue editing normally
   - Status shows queued operations: `Offline - queued 5 ops`

3. **Restore Connection**
   - Press any key to trigger reconnection
   - Status shows `[Syncing...] v50`
   - Client receives missed operations from other users
   - Client replays your offline edits
   - Status returns to `[Connected] v67`
   - Your work is preserved!

## 🏗️ Architecture

### Client-Server Model
- **Centralized Server**: Single source of truth for document state
- **Thin Clients**: Render UI and handle user input
- **Operation-Based Sync**: Changes transmitted as discrete operations

### Protocol
- **Fixed-Size Messages**: `NetMessage` struct (defined in `common.h`)
- **Operation Types**: Insert, Delete, Format, Cursor Move, Selection, etc.
- **Version Tracking**: Each operation tagged with document version
- **Binary Protocol**: Direct struct serialization over TCP

### Synchronization Strategy

#### Normal Operation (Connected)
```
Client Operation → Server → Validate → Apply → Broadcast to All Clients
```

#### Reconnection (Incremental Sync)
```
1. Client sends: current version + document hash
2. Server compares versions and hashes
3. If client behind: Server sends missed operations
4. Client applies missed operations
5. Client replays offline operations
6. Server broadcasts replayed operations
```

#### Reconnection (Full Sync)
```
1. Client version too old OR hash mismatch
2. Server sends complete document state
3. Client discards offline operations (conflict too complex)
4. Client resets to server state
```

### Conflict Resolution
- **Last-Write-Wins**: Operations applied in server-received order
- **Position-Based**: Concurrent edits at different positions don't conflict
- **Deterministic**: All clients converge to identical state

## 🔒 Security Considerations

⚠️ **WARNING**: This editor includes code execution capabilities.

### Security Risks
- **Arbitrary Code Execution**: Any client can run C code on the server
- **No Authentication**: No user verification or access control
- **No Encryption**: All data transmitted in plaintext
- **No Sandboxing**: Executed code runs with server process privileges

### Safe Usage
- ✅ **Trusted LAN Only**: Use only on isolated, trusted networks
- ✅ **Known Users**: Only allow connections from trusted users
- ✅ **Firewall**: Block port 8080 from external networks
- ✅ **Dedicated Machine**: Run on a machine without sensitive data
- ❌ **Never expose to Internet** without adding authentication and sandboxing

### Recommended Improvements for Production
- Add user authentication (username/password or key-based)
- Implement TLS/SSL encryption
- Sandbox code execution (Docker, chroot, or seccomp)
- Add rate limiting
- Implement access control lists (ACLs)
- Add audit logging

## 🐛 Troubleshooting

### Client Can't Connect
```
Error: Connection Failed
```
**Solutions:**
- Check server is running: `ps aux | grep server`
- Verify IP address: Must match server output
- Check firewall: `sudo ufw allow 8080/tcp` (Ubuntu)
- Try localhost first: `./client 127.0.0.1`

### Garbled Display
**Solutions:**
- Check terminal size: Minimum 80x24 recommended
- Verify ncurses installation: `ldconfig -p | grep ncurses`
- Try different terminal: xterm, gnome-terminal, iTerm2
- Check TERM variable: `echo $TERM`

### Code Execution Fails
```
Error: Code block ```...``` not found.
```
**Solutions:**
- Ensure code is wrapped in triple backticks: `` ``` ``
- Check GCC is installed: `gcc --version`
- Verify code syntax is valid C
- Check server terminal for compilation errors

### Reconnection Issues
**Solutions:**
- Wait 2-3 seconds after network restore
- Press any key to trigger reconnection
- Check server is still running
- If full resync required, offline edits will be lost (by design)

### Performance Issues
**Symptoms:** Lag, slow updates, choppy cursor movement

**Solutions:**
- Reduce number of clients (max 10 supported)
- Check network latency: `ping <server_ip>`
- Close other network-intensive applications
- Reduce document size (current max: 1024 lines × 1024 chars)

## 📊 Limitations

### Current Limits
- **Maximum Clients**: 10 simultaneous users
- **Document Size**: 1024 lines × 1024 characters per line
- **Offline Queue**: 100 operations
- **Operation History**: 1000 operations (for incremental sync)
- **Network Protocol**: TCP only (no UDP option)

### Known Issues
- No undo/redo functionality
- No file save/load (document is memory-only)
- No clipboard/copy-paste support
- Selection limited to keyboard (no mouse support)
- Terminal resizing not handled gracefully
- No syntax highlighting

## 🛣️ Roadmap

### Planned Features
- [ ] File persistence (save/load from disk)
- [ ] Undo/redo with operation log
- [ ] User authentication system
- [ ] TLS encryption
- [ ] Mouse support for selection
- [ ] Syntax highlighting
- [ ] Multiple document support
- [ ] Chat/messaging sidebar
- [ ] Presence indicators (user list)
- [ ] Document history/versioning

### Performance Improvements
- [ ] Compressed operation protocol
- [ ] Delta encoding for large operations
- [ ] Efficient line rendering (only changed lines)
- [ ] Operation batching

## 🤝 Contributing

Contributions are welcome! Areas needing improvement:
- Security hardening
- Performance optimization
- Additional text operations (copy/paste, find/replace)
- Better error handling
- Test suite

## 📄 License

MIT License - See LICENSE file for details

## 👥 Authors

Created as a demonstration of real-time collaborative editing with operational transformation concepts.

## 🙏 Acknowledgments

- Built with ncurses for terminal UI
- Inspired by collaborative editors like Google Docs, Etherpad
- Operational Transformation concepts from Jupiter/Wave research

## 📞 Support

For issues, questions, or feature requests:
- Open an issue on GitHub
- Check troubleshooting section above
- Review architecture documentation

----

**Happy Collaborative Editing! 🎉**

## AI Chats
https://claude.ai/chat/dbd379da-2757-4e02-aa81-57000814b190
https://chatgpt.com/c/68eb0203-3308-8323-9dcc-a6f0f96237b1
https://gemini.google.com/app/64a03661d1d02663
https://gemini.google.com/app/f0a5a7d2adb4e445
