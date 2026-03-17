// g++ pipes2.cpp -o pipes2 -Wall -Wextra -fuse-ld=lld -Wshadow -g -fsanitize=address,undefined -O3 -std=c++20
// I should put an intro but test it to understand what it is

#include <chrono>
#include <thread>
#include <vector>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cmath>
#include <unistd.h>

#define SHADES 116 // Change this if you change the multiplier for the shading
#define DEBUG // (comment to disable)
// You might want to use with this option:
// errout=$(mktemp) && ./pipes2 2> $errout ; printf "\033[H\033[0m" ; cat $errout

#define PIPE_PER_CHUNK 147 // One chunk is 64*64
                           // (chunk means nothing in the logic)
#define MAX_TERM_WIDTH 1000 // If you change this you will need to change
                            // the LUT and the write function

#define MEMCPY __builtin_memcpy // You may use memcpy if you have bugs
#define MEMSET __builtin_memset //             memset
#define MEMCMP __builtin_memcmp //             memcmp
#define RAND    rand

////////////////////////////////////////////////////////////////////////////////

class Pipes;
class Pipe;

void get_terminal_size(int &, int &);
std::array<uint8_t, 3> query_term_bg();
consteval std::array<uint8_t,256 * 3> make_hue_LUT();
consteval std::size_t digits10(size_t);
template <size_t MAX> consteval auto make_num_to_char_lut();
consteval std::array<uint8_t, SHADES> make_decay_lut();

static constexpr char SYMBOLS[3*6+1] = "│─╭╮╰╯"; // UTF8: 3 bytes each + \0
// Each NEEDS to be 3 byte each otherwise the print function will break.
// Add \0 if you need

////////////////////////////////////////////////////////////////////////////////

// Thanks cross-platform compat...
// (thanks ProjectPhysX)
#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	define VC_EXTRALEAN
#	include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#	include <sys/ioctl.h>
#	include <termios.h>
#	include <fcntl.h>
#endif // Windows/Linux

void get_terminal_size(int &width, int &height)
{
#if defined(_WIN32)
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	width = (int)(csbi.srWindow.Right-csbi.srWindow.Left+1);
	height = (int)(csbi.srWindow.Bottom-csbi.srWindow.Top+1);
#elif defined(__linux__) || defined(__APPLE__)
	struct winsize w;
	ioctl(fileno(stdout), TIOCGWINSZ, &w);
	width = (int)(w.ws_col);
	height = (int)(w.ws_row);
#endif // Windows/Linux
}

// I don't understand anything about this function except the core mecanism
// ("\033]11;?\007")
// It asks the terminal its background color
std::array<uint8_t, 3> query_term_bg()
{
	char buf[64]; int len = 0;

#ifdef _WIN32 // ── Windows ────────────────────────────────────────────────────
	HANDLE hi = CreateFileA("CONIN$",  GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ,  nullptr, OPEN_EXISTING, 0, nullptr);
	HANDLE ho = CreateFileA("CONOUT$", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hi == INVALID_HANDLE_VALUE || ho == INVALID_HANDLE_VALUE) {
		if (hi != INVALID_HANDLE_VALUE) CloseHandle(hi);
		if (ho != INVALID_HANDLE_VALUE) CloseHandle(ho);
		return {};
	}
	DWORD mi, mo;
	GetConsoleMode(hi, &mi); GetConsoleMode(ho, &mo);
	SetConsoleMode(ho, mo | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	SetConsoleMode(hi, (mi & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT)) | ENABLE_VIRTUAL_TERMINAL_INPUT);

	DWORD nw; WriteConsoleA(ho, "\033]11;?\007", 7, &nw, nullptr);

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
	while (len < 63) {
		auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (rem <= 0) break;
		if (WaitForSingleObject(hi, (DWORD)rem) != WAIT_OBJECT_0) break;
		INPUT_RECORD ir; DWORD nr;
		if (!ReadConsoleInputA(hi, &ir, 1, &nr)) break;
		if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown && ir.Event.KeyEvent.uChar.AsciiChar)
			buf[len++] = ir.Event.KeyEvent.uChar.AsciiChar;
		if (len && buf[len - 1] == '\007') break;
	}
	SetConsoleMode(hi, mi); SetConsoleMode(ho, mo);
	CloseHandle(hi); CloseHandle(ho);


#else // ── POSIX (Linux / macOS) ──────────────────────────────────────────────
	int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
	if (fd < 0) return {};

	struct termios t0, t1;
	tcgetattr(fd, &t0); t1 = t0;
	cfmakeraw(&t1); t1.c_cc[VMIN] = 0; t1.c_cc[VTIME] = 0;
	tcsetattr(fd, TCSAFLUSH, &t1);

	write(fd, "\033]11;?\007", 7);

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
	while (len < 63) {
		if (std::chrono::steady_clock::now() >= deadline) break;
		char c;
		if (read(fd, &c, 1) == 1) {
			buf[len++] = c;
			if (c == '\007') break;
		}
		else std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	tcsetattr(fd, TCSAFLUSH, &t0);
	close(fd);
#endif

	// Parse "rgb:RRRR/GGGG/BBBB"
	buf[len] = 0;
	const char *p = strstr(buf, "rgb:");
	if (!p) return {};
	p += 4;
	const char *sl = strchr(p, '/');
	if (!sl) return {};
	int sh = ((int)(sl - p) - 2) * 4; // 4 hex digits → shift 8, 2 digits → shift 0
	unsigned r, g, b;
	if (sscanf(p, "%x/%x/%x", &r, &g, &b) != 3) return {};
	if (sh < 0) sh = 0;
	return { uint8_t(r >> sh), uint8_t(g >> sh), uint8_t(b >> sh) };
}

// Converts HSV hue (brightness and value are 255) to RGB color
// You need to use r=LUT[3*i+0] g=LUT[3*i+1] b=[3*i+2]
consteval std::array<uint8_t,256 * 3> make_hue_LUT()
{
	std::array<uint8_t,256 * 3> lut;

	for (uint_fast16_t i = 0; i < 256; i++) {
		size_t idx = i * 3;
		uint8_t sector = i / 43;
		uint8_t offset = i - sector * 43;

		uint8_t t = ((uint16_t)offset * 255 + 21) / 43;

		switch (sector)	{
			case 0: lut[idx]=255;  lut[idx+1]=t;    lut[idx+2]=0;    break;
			case 1: lut[idx]=255-t;lut[idx+1]=255;  lut[idx+2]=0;    break;
			case 2: lut[idx]=0;    lut[idx+1]=255;  lut[idx+2]=t;    break;
			case 3: lut[idx]=0;    lut[idx+1]=255-t;lut[idx+2]=255;  break;
			case 4: lut[idx]=t;    lut[idx+1]=0;    lut[idx+2]=255;  break;
			default:lut[idx]=255;  lut[idx+1]=0;    lut[idx+2]=255-t;break;
		}
	}
	return lut;
}

consteval std::size_t digits10(size_t v)
{
	std::size_t d = 1;
	while (v >= 10) {
		v /= 10;
		++d;
	}
	return d;
}

template <size_t MAX>
consteval auto make_num_to_char_lut()
{
	constexpr std::size_t DIGITS = digits10(MAX);

	std::array<char[DIGITS], MAX + 1> lut{};

	for (std::size_t i = 0; i <= MAX; ++i) {
		std::size_t v = i;

		for (std::size_t d = 0; d < DIGITS; ++d) {
			auto idx = DIGITS - 1 - d;
			lut[i][idx] = char('0' + (v % 10));
			v /= 10;
		}
	}

	return lut;
}

// Precompute decay factors: (250/255)^i for each shade
// Change SHADES if you change the factor
consteval std::array<uint8_t, SHADES> make_decay_lut()
{
	std::array<uint8_t, SHADES> lut;
	uint32_t value = 255;  // Start at full brightness
	for (size_t i = 0; i < SHADES; i++) {
		lut[SHADES-i-1] = (uint8_t)value;
		value = value * 250 / 255;  // Apply one decay step
	}
	return lut;
}

////////////////////////////////////////////////////////////////////////////////

// L1 cache alignment (64-byte cache line) for hot lookup tables
alignas(64) static constexpr std::array<uint8_t,256*3> HUE_LUT = make_hue_LUT();
alignas(64) static constexpr auto DECAY_LUT = make_decay_lut();
alignas(64) static constexpr auto NUM_TO_CHAR_LUT_3 = make_num_to_char_lut<999>();
//alignas(64) static constexpr auto NUM_TO_CHAR_LUT_4 = make_num_to_char_lut<9999>();

////////////////////////////////////////////////////////////////////////////////

class Pipe
{
public:
	Pipes *parent;
	uint16_t headX = 0;
	uint16_t headY = 0;
	uint8_t direction; // 0 north, 1 east, 2 south, 3 west
	uint8_t rgb[3] = {0};
	static constexpr uint8_t rotProba = 20; // out of 128
	// Value is adjusted later

	uint16_t pipePos[SHADES*2] = {0}; // Stores XY for each pipe already drawn.
	// 116 is the number of shades of the color (starting from 255) that i
	// will get if i fo x * 250 / 255 after each frame
	// so at frame 117 the first pixel will be black
	uint8_t charId[SHADES] = {0};
	uint_fast8_t index = 0; // Index of the last pixel drawn

	Pipe(Pipes *p);
	void move();
	void reprint(uint_fast8_t localIndex);
};

class Pipes
{
private:
	std::vector<Pipe> pipes;
	unsigned density = 4;
public:
	int terminalHeight = 0;
	int terminalWidth = 0;
	std::array<uint8_t, 3> bgRgb;
	size_t BUFFER_SIZE = 0;
	uint8_t (*framebuf)[2][3] = nullptr; // [terminalWidth*y+x][set1/2][r/g/b or utf8]

	Pipes(std::array<uint8_t, 3> bg = {0}):
		bgRgb(bg)
	{
		get_terminal_size(terminalWidth, terminalHeight);
		BUFFER_SIZE = terminalHeight * terminalWidth;
		// Controls the density of pipes drawn (depends on the size of the screen)
		// Change the divider  to match your preferences
		density = std::max(BUFFER_SIZE / PIPE_PER_CHUNK,(size_t)4);

		framebuf = new uint8_t[BUFFER_SIZE][2][3]{0};
		
		for (unsigned i = 0; i < density; i++)
			pipes.emplace_back(Pipe(this));
	}

	~Pipes()
	{
		delete framebuf;
	}

	// Next frame
	void next_frame()
	{
		for (auto &pipe : pipes)
			pipe.move();
		MEMSET(framebuf, 0, BUFFER_SIZE * 6);
		for (uint_fast8_t i = 0; i < SHADES; i++)
			for (auto &pipe : pipes)
				pipe.reprint(i);
	}

	// Append to the buffer
	inline void write_to_buf(
		const uint8_t rgb[3],
		const char utf8[3],
		const uint16_t x,
		const uint16_t y)
	{
		size_t pos = y * terminalWidth + x;
		if (pos >= BUFFER_SIZE) [[unlikely]]
		{
			#ifdef DEBUG
			fprintf(stderr, "Error: bad buffer pos (overflow)\n");
			#endif
			return;
		}
		MEMCPY(framebuf[pos][0], rgb, 3);
		MEMCPY(framebuf[pos][1], utf8, 3);
	}

	// Write the buffer to the terminal
	inline void flush_buf()
	{
		// Each char is a color ESC code "\033[38;2;000;000;000m" 
		// + the actual UTF-8 char (3 byte) so 19 + 3 = 22 bytes per char
		// + the "\033[000;0H" (8 byte) to move to the correct pos
		const size_t lineLen = terminalWidth*22 + 8;
		static char line[MAX_TERM_WIDTH*22 + 8];
		static constexpr char MV[9] = "\033[000;0H"; // 8+\0 but we dont need \0
		static constexpr char COLOR[20] = "\033[38;2;000;000;000m"; // 19+\0
		static constexpr char SKIP[7] = "\033[000C"; // 6+\0
		for (uint_fast16_t y = 0; y < (uint_fast16_t)terminalHeight; y++)
		{
			size_t lenLineAct = lineLen;
			// Reset line
			MEMSET(line, 0, lineLen);
			// Init line
			MEMCPY(line, MV, 8);
			MEMCPY(line + 2, NUM_TO_CHAR_LUT_3[y+1], 3);
			char *start = line+8;
			uint16_t nCellsSkipped = 0;
			for (uint_fast16_t x = 0; x < (uint_fast16_t)terminalWidth; x++)
			{
				uint8_t (*cell)[3] = framebuf[y*terminalWidth+x];
				// Check if RGB is black + char is null
				if (!(cell[0][0] || cell[0][1] || cell[0][2] || cell[1][0])) {
					nCellsSkipped++;
					lenLineAct -= 22; // Remember that we skipped 22 bytes
					continue;
				} else if (nCellsSkipped) { // Write that we skipped cells
				                            // before writting the new bytes
					MEMCPY(start, SKIP, 6);
					MEMCPY(start + 2, NUM_TO_CHAR_LUT_3[nCellsSkipped], 3);
					start += 6;
					lenLineAct += 6;
					nCellsSkipped = 0;
				}
				MEMCPY(start, COLOR, 19);
				// Write RGB (r at 17..19, g at 21..23, b at 25..27)
				MEMCPY(start + 7,  NUM_TO_CHAR_LUT_3[cell[0][0]], 3);
				MEMCPY(start + 11, NUM_TO_CHAR_LUT_3[cell[0][1]], 3);
				MEMCPY(start + 15, NUM_TO_CHAR_LUT_3[cell[0][2]], 3);
				// Write the char
				MEMCPY(start + 19, cell[1], 3);
				start += 22;
			}

			write(1, line, lenLineAct);
		}
	}
};

Pipe::Pipe(Pipes *p) :
		parent(p),
		headX(RAND() % parent->terminalWidth),
		headY(RAND() % parent->terminalHeight),
		direction(RAND() & 0b11)
{
	const uint8_t *val = HUE_LUT.data() + (RAND() & 0xFF) * 3;
	rgb[0] = *val, rgb[1] = *++val, rgb[2] = *++val;
}

void Pipe::move()
{
	// The logic is around 5 tasks:
	// - select the correct symbol to print
	// - change the direction randomly
	// - go in this direction
	// - append the char pos to the list so it can be modified later


	///----- HEAD PLACEMENT -------------------------------------------------///
	// For each dir, + of - in the correct direction
	// then % to stay in the terminal
	switch (direction) {
	case 0: // North
		(headY += parent->terminalHeight-1) %= parent->terminalHeight;
		break;
	case 2: // South
		++headY %= parent->terminalHeight;
		break;
	case 3: // West
		(headX += parent->terminalWidth-1) %= parent->terminalWidth;
		break;
	case 1: // East
		++headX %= parent->terminalWidth;
		break;
	default: [[unlikely]]
		fprintf(stderr, "Direction error\n");
		break;
	}

	///----- Change direction -----------------------------------------------///
	// For the next frame

	// Check if the pipe can turn or if it will overlap an other pipe
	// (only crossing allowed)
	// Logic is simple: if you are on a pipe you cant turn
	// you must go forward
	// this might cause issues:
	//         ||
	// ========>`----2
	//         |
	//         1
	// In that case, the new pipe (===>) will overlap the 2nd pipe
	uint8_t (*cell)[3] = parent->framebuf[parent->terminalWidth*headY+headX];
	static constexpr uint8_t DIR_CW_LUT[] = {6, 9, 15, 12};
	static constexpr uint8_t DIR_CCW_LUT[] = {9, 15, 12, 6};
	if (!(cell[0][0] || cell[0][1] || cell[0][2]))
	{
		// If the movement is horizontal, the chances to change dir are divided
		// by 2 because the format of a monospace char is 2:1
		uint8_t randN = RAND() & 0xFF;
		// If direction is odd the mov is horizontal
		// so we multiply rot proba by 2
		uint8_t compProb = (rotProba << ((~direction)&1)) - 1;
		if (randN <= compProb) { // direction move CW
			charId[index] = DIR_CW_LUT[direction];
			++direction &= 0b11;
		} else if (randN >= 0xFF - compProb) { // direction move CCW
			charId[index] = DIR_CCW_LUT[direction];
			(direction += 3) &= 0b11;
		} else {
			/* FIXME
			// Avoid going on a corner pipe
			uint16_t x = headX, y = headY;
			switch (direction) { // It will still be able to go on corners
			                     // but less frequently (only on turns)
			case 0: (y += parent->terminalHeight-1) %= parent->terminalHeight; break; // North
			case 2: ++y %= parent->terminalHeight; break; // South
			case 3: (x += parent->terminalWidth-1) %= parent->terminalWidth; break; // West
			case 1: ++x %= parent->terminalWidth; break; // East
			default: [[unlikely]]
				fprintf(stderr, "Direction error\n");
				break;
			}
			// Check if it's a straight pipe
			if (MEMCMP(&SYMBOLS[3*1], (const char*)parent->framebuf[parent->terminalWidth*y+x][1], 3)
			 || MEMCMP(&SYMBOLS[3*0], (const char*)parent->framebuf[parent->terminalWidth*y+x][1], 3)) {
				charId[index] = 3*(direction&1);
			} else {
				if (RAND() & 1) { // direction move CW
					charId[index] = DIR_CW_LUT[direction];
					++direction &= 0b11;
				} else { // direction move CCW
					charId[index] = DIR_CCW_LUT[direction];
					(direction += 3) &= 0b11;
				}
			}*/
			charId[index] = 3*(direction&1);
		}
	} else if (!MEMCMP(&SYMBOLS[3*(direction&1)], (const char*)cell[1], 3)) {
		// When already on a pipe
		// There is no question, you WILL turn NOW
		if (RAND() & 1) { // direction move CW
			charId[index] = DIR_CW_LUT[direction];
			++direction &= 0b11;
		} else { // direction move CCW
			charId[index] = DIR_CCW_LUT[direction];
			(direction += 3) &= 0b11;
		}
	} else {
		charId[index] = 3*(direction&1);
	}

	///----- MOVEMENT -------------------------------------------------------///
	pipePos[index * 2] = headX;
	pipePos[index * 2 + 1] = headY;
	index = (index + 1) % SHADES;
}

// Local index because its the priority:
// lowers (first drawn/oldest) to higher (last drawn/newest)
// localIndex=0: oldest, localIndex=SHADES-1: newest
void Pipe::reprint(uint_fast8_t localIndex)
{
	uint_fast8_t idx = (index + localIndex) % SHADES;
	// localIndex directly indexes decay table
	uint8_t decay = DECAY_LUT[localIndex];
	uint8_t invDecay = 255 - decay;
	uint8_t newRgb[3] = {
		(uint8_t)(((rgb[0]*decay)>>8) + ((parent->bgRgb[0]*invDecay)>>8)),
		(uint8_t)(((rgb[1]*decay)>>8) + ((parent->bgRgb[1]*invDecay)>>8)),
		(uint8_t)(((rgb[2]*decay)>>8) + ((parent->bgRgb[2]*invDecay)>>8))
	};
	uint16_t posX = pipePos[idx * 2];
	if (!posX) return;
	parent->write_to_buf(newRgb, &SYMBOLS[charId[idx]], posX, pipePos[idx*2+1]);
}

////////////////////////////////////////////////////////////////////////////////

int main()
{
	srand(time(NULL));

	// Startup time is due to this
	std::array<uint8_t, 3> bgRgb = query_term_bg();

	Pipes pipes(bgRgb);
	using clock = std::chrono::steady_clock;
    constexpr auto frame_time = std::chrono::milliseconds(50); // 20 FPS

	auto next = clock::now();

	#ifdef DEBUG
	fprintf(stderr, "\nDebug mode enabled\n");
	#endif

	// Set bold and hide cursor
	write(1, "\033[1m\033[?25l", 11);

	while (true) {
		next += frame_time;

		#ifdef DEBUG
		// Measure render time
		auto frame_start = clock::now();
		#endif
		pipes.next_frame();
		pipes.flush_buf();
		#ifdef DEBUG
		auto frame_end = clock::now();
		
		// Debug: render time in top-right corner
		auto render_us = std::chrono::duration_cast<std::chrono::microseconds>(
			frame_end - frame_start).count();
		char debug_buf[64];
		int len = snprintf(debug_buf, sizeof(debug_buf),
			"\033[1;70H\033[38;2;255;255;255m%5ld µs / 50'000µs",
			render_us);
		if (len > 0)
			write(1, debug_buf, len);
		#endif

		std::this_thread::sleep_until(next);
	}
	return 0;
}
