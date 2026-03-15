// g++ pipes2.cpp -o pipes2 -Wall -Wextra -fuse-ld=lld -Wshadow -g -fsanitize=address,undefined -O3 -std=c++20

#include <chrono>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <array>
#include <cstring>
#include <unistd.h>
#include <cassert>

#define DO_SHADES
#define DEBUG
#define SHADES 116
#define PIPES_COUNT 32
#define FRAME_BUFFER 131072 // Should me more than 34*PIPES_COUNT*SHADES
                            // 34 being the buffer of print_char_at_rgb
#define MEMCPY __builtin_memcpy // You can use memcpy if you have bugs

////////////////////////////////////////////////////////////////////////////////

static constexpr char SYMBOLS[3*6+1] = "│─╭╮╰╯"; // UTF8: 3 bytes each + \0
char framebuf[FRAME_BUFFER];
unsigned framebufLen = 0;

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

inline ssize_t flush_buf()
{
	ssize_t r = write(1, framebuf, framebufLen);
	framebufLen = 0;
	return r;
}

// Append to the buffer
// Note: Caller must ensure n < FRAME_BUFFER or this will overflow
inline void write_to_buf(const char *buf, size_t n)
{
	if (n + framebufLen >= FRAME_BUFFER) [[unlikely]]
		flush_buf();
	MEMCPY(framebuf + framebufLen, buf, n);
	framebufLen += n;
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

// L1 cache alignment (64-byte cache line) for hot lookup tables
alignas(64) static constexpr std::array<uint8_t,256 * 3> HUE_LUT =
	make_hue_LUT();

// Precompute decay factors: (250/255)^i for each shade
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

alignas(64) static constexpr auto DECAY_LUT = make_decay_lut();

alignas(64) static constexpr auto NUM_TO_CHAR_LUT_3 =
	make_num_to_char_lut<999>();
//alignas(64) static constexpr auto NUM_TO_CHAR_LUT_4 =
//	make_num_to_char_lut<9999>();

inline void print_char_at_rgb(
	uint16_t x,
	uint16_t y,
	const unsigned char rgb[3],
	const char c[4])
{
	//char buf[35] = "\033[000;0000H\033[38;2;000;000;000m \0\0\0";
	char buf[34] = "\033[000;000H\033[38;2;000;000;000m \0\0\0";
		// for cursor + RGB + char (utf8 so 4bytes) + \0

    // Overwrite the coordinates (y at 3..5, x at 7..9)
    MEMCPY(buf + 2, NUM_TO_CHAR_LUT_3[y], 3);
    MEMCPY(buf + 6, NUM_TO_CHAR_LUT_3[x], 3);

    // Overwrite RGB (r at 17..19, g at 21..23, b at 25..27)
    MEMCPY(buf + 17, NUM_TO_CHAR_LUT_3[rgb[0]], 3);
    MEMCPY(buf + 21, NUM_TO_CHAR_LUT_3[rgb[1]], 3);
    MEMCPY(buf + 25, NUM_TO_CHAR_LUT_3[rgb[2]], 3);

	// Character
    MEMCPY(buf + 29, c, 3);

	// Write all at once
	write_to_buf(buf, 34);
}

////////////////////////////////////////////////////////////////////////////////

class Pipe
{
public:
	//inline static unsigned idIncr = 0;
	//unsigned id;
	uint16_t headX = 0;
	uint16_t headY = 0;
	uint8_t direction; // 0 north, 1 east, 2 south, 3 west
	uint8_t rgb[3] = {0};
	std::array<uint8_t, 3> bgRgb;
	int *termWidth;
	int *termHeight;
	static constexpr uint8_t rotProba = 20; // out of 128
	// Value is adjusted later

	#ifdef DO_SHADES
	uint16_t pipePos[SHADES*2] = {0}; // Stores XY for each pipe already drawn.
	// 116 is the number of shades of the color (starting from 255) that i
	// will get if i fo x * 250 / 255 after each frame
	// so at frame 117 the first pixel will be black
	uint8_t charId[SHADES] = {0};
	uint_fast8_t index = 0; // Index of the last pixel drawn
	#endif

	Pipe(int *w, int *h, std::array<uint8_t, 3> &bg) :
	//	id(idIncr++),
		headX(rand() % *w), headY(rand() % *h),
		direction(rand() & 0b11),
		bgRgb(bg),
		termWidth(w), termHeight(h)
	{
		const uint8_t *val = HUE_LUT.data() + (rand() & 0xFF) * 3;
		rgb[0] = *val, rgb[1] = *++val, rgb[2] = *++val;
	}

	void move()
	{
		// The logic is around 4 tasks:
		// - select the correct symbol to print
		// - change the direction randomly
		// - go in this direction
		// - append the char pos to the list so it can be modified later

		char symb[4] = "+";

		// Change direction
		// If the movement is horizontal, the chances to change dir are divided
		// by 2 because the format of a monospace char is 2:1
		uint8_t randN = rand() & 0xFF;
		// If direction is odd the mov is horizontal
		// so we multiply rot proba by 2
		uint8_t compProb = (rotProba << ((~direction)&1)) - 1;
		if (randN <= compProb) { // direction move CW
			static constexpr uint8_t DIR_LUT[] = {6, 9, 15, 12};
			MEMCPY(symb, &SYMBOLS[charId[index] = DIR_LUT[direction]], 3);
			/* Same as
			switch (direction) {
			case 0: // South -> East
				memcpy(symb, &SYMBOLS[charId[index] = 2*3], 3); break;
			case 1: // West -> South
				memcpy(symb, &SYMBOLS[charId[index] = 3*3], 3); break;
			case 2: // North -> West
				memcpy(symb, &SYMBOLS[charId[index] = 5*3], 3); break;
			case 3: // East -> North
				memcpy(symb, &SYMBOLS[charId[index] = 4*3], 3); break;
			default:
				fprintf(stderr, "Direction error");
				break;
			}*/
			++direction &= 0b11;
		} else if (randN >= 0xFF - compProb) { // direction move CCW
			static constexpr uint8_t DIR_LUT[] = {9, 15, 12, 6};
			MEMCPY(symb, &SYMBOLS[charId[index] = DIR_LUT[direction]], 3);
			/* Same as
			switch (direction) {
			case 0: // South -> West
				memcpy(symb, &SYMBOLS[charId[index] = 3*3], 3); break;
			case 1: // West -> North
				memcpy(symb, &SYMBOLS[charId[index] = 5*3], 3); break;
			case 2: // North -> East
				memcpy(symb, &SYMBOLS[charId[index] = 4*3], 3); break;
			case 3: // East -> South
				memcpy(symb, &SYMBOLS[charId[index] = 2*3], 3); break;
			default:
				fprintf(stderr, "Direction error");
				break;
			}*/
			(direction += 3) &= 0b11;
		} else {
			MEMCPY(symb, &SYMBOLS[charId[index] = 3*(direction&1)], 3);
		}

		#ifdef DO_SHADES
			pipePos[index * 2] = headX;
			pipePos[index * 2 + 1] = headY;
			index = (index + 1) % SHADES;
		#else
			print_char_at_rgb(headX, headY, rgb, symb);
		#endif

		// For each dir, + of - in the correct direction
		// then % to stay in the terminal
		switch (direction) {
		case 0: // North
			(headY += *termHeight-1) %= *termHeight;
			break;
		case 2: // South
			++headY %= *termHeight;
			break;
		case 3: // West
			(headX += *termWidth-1) %= *termWidth;
			break;
		case 1: // East
			++headX %= *termWidth;
			break;
		default: [[unlikely]]
			fprintf(stderr, "Direction error");
			break;
		}
	}

	// Local index because its the priority:
	// lowers (first drawn/oldest) to higher (last drawn/newest)
	// localIndex=0: oldest, localIndex=SHADES-1: newest
	void reprint(uint_fast8_t localIndex)
	{
		uint_fast8_t idx = (index + localIndex) % SHADES;
		// localIndex directly indexes decay table
		uint8_t decay = DECAY_LUT[localIndex];
		uint8_t invDecay = 255 - decay;
		uint8_t newRgb[3] = {
			(uint8_t)(((rgb[0]*decay) >> 8) + ((bgRgb[0]*invDecay) >> 8)),
			(uint8_t)(((rgb[1]*decay) >> 8) + ((bgRgb[1]*invDecay) >> 8)),
			(uint8_t)(((rgb[2]*decay) >> 8) + ((bgRgb[2]*invDecay) >> 8))
		};
		uint16_t posX = pipePos[idx * 2];
		if (!posX) return;
		print_char_at_rgb(
			posX,
			pipePos[idx * 2 + 1],
			newRgb,
			&SYMBOLS[charId[idx]]);
	}
};

class Pipes
{
private:
	std::vector<Pipe> pipes;
	std::array<uint8_t, 3> backgroundRgb;
public:
	int terminalHeight = 0;
	int terminalWidth = 0;

	Pipes(std::array<uint8_t, 3> bgRgb = {0}):
		backgroundRgb(bgRgb)
	{
		get_terminal_size(terminalWidth, terminalHeight);
		
		for (uint_fast16_t i = 0; i < PIPES_COUNT; i++)
			pipes.emplace_back(
				Pipe(&terminalWidth, &terminalHeight, backgroundRgb));
	}

	// Next frame
	void next_frame()
	{
		for (auto &pipe : pipes)
			pipe.move();
		#ifdef DO_SHADES
		for (uint_fast8_t i = 0; i < SHADES; i++)
			for (auto &pipe : pipes)
				pipe.reprint(i);
		#endif
	}
};

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

	// Set bold and hide cursor
	write(1, "\033[1m\033[?25l", 11);

	while (true) {
		next += frame_time;

		#ifdef DEBUG
		// Measure render time
		auto frame_start = clock::now();
		#endif
		pipes.next_frame();
		write_to_buf("\033[0;0H", 6);
		flush_buf();
		#ifdef DEBUG
		auto frame_end = clock::now();
		
		// Debug: render time in top-right corner
		auto render_us = std::chrono::duration_cast<std::chrono::microseconds>(
			frame_end - frame_start).count();
		char debug_buf[64];
		int len = snprintf(debug_buf, sizeof(debug_buf),
			"\033[1;70H\033[38;2;255;255;255m%5ld µs / 50'000µs",
			render_us);
		if (len > 0) write_to_buf(debug_buf, len);
		flush_buf();
		#endif

		std::this_thread::sleep_until(next);
	}
	return 0;
}
