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

////////////////////////////////////////////////////////////////////////////////

static constexpr char8_t SYMBOLS[] = u8"│─╭╮╰╯"; 

// Thanks cross-platform compat...
// (thanks ProjectPhysX)
#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	define VC_EXTRALEAN
#	include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#	include <sys/ioctl.h>
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
			case 0:  lut[idx]=255;  lut[idx+1]=t;    lut[idx+2]=0; break;
			case 1:  lut[idx]=255-t;lut[idx+1]=255;  lut[idx+2]=0; break;
			case 2:  lut[idx]=0;    lut[idx+1]=255;  lut[idx+2]=t; break;
			case 3:  lut[idx]=0;    lut[idx+1]=255-t;lut[idx+2]=255; break;
			case 4:  lut[idx]=t;    lut[idx+1]=0;    lut[idx+2]=255; break;
			default: lut[idx]=255;  lut[idx+1]=0;    lut[idx+2]=255-t; break;
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

static constexpr std::array<uint8_t,256 * 3> HUE_LUT = make_hue_LUT();

/* DEPRECATED: slow
// Top left has coords 0;0. XY order (width, height)
// Use it like printf
inline int print_at(int w, int h, const char *fmt, ...)
{
	char combined_fmt[512];
	snprintf(
		combined_fmt, sizeof(combined_fmt),
		"\033[%i;%iH%s", h - 1, w - 1, fmt);
	
	va_list args;
	va_start(args, fmt);
	int r = vprintf(combined_fmt, args);
	va_end(args);

	return r;
}
// Same as print_at but with color
inline int print_at_rgb(int w, int h, const char rgb[3], const char *fmt, ...)
{
	char combined_fmt[512];
	snprintf(
		combined_fmt, sizeof(combined_fmt),
		"\033[%i;%iH\033[38;2;%hhu;%hhu;%hhum%s",
		     h-1,w-1,  rgb[0],rgb[1],rgb[2], fmt);
	
	va_list args;
	va_start(args, fmt);
	int r = vprintf(combined_fmt, args);
	va_end(args);

	return r;
}*/

static constexpr auto NUM_TO_CHAR_LUT_3 = make_num_to_char_lut<999>();
static constexpr auto NUM_TO_CHAR_LUT_4 = make_num_to_char_lut<9999>();

inline void print_char_at_rgb(
	uint16_t x,
	uint16_t y,
	const unsigned char rgb[3],
	const char c[4])
{
	char buf[35] = "\033[000;0000H\033[38;2;000;000;000m \0\0\0";
		// for cursor + RGB + char (utf8 so 4bytes) + \0

    // Overwrite the coordinates (y at 3..5, x at 7..9)
    memcpy(buf + 2, NUM_TO_CHAR_LUT_3[y], 3);
    memcpy(buf + 6, NUM_TO_CHAR_LUT_4[x], 4);

    // Overwrite RGB (r at 17..19, g at 21..23, b at 25..27)
    memcpy(buf + 18, NUM_TO_CHAR_LUT_3[rgb[0]], 3);
    memcpy(buf + 22, NUM_TO_CHAR_LUT_3[rgb[1]], 3);
    memcpy(buf + 26, NUM_TO_CHAR_LUT_3[rgb[2]], 3);

	// Character
    memcpy(buf + 30, c, 3);

	// Write all at once
	write(1, buf, 35);
}

////////////////////////////////////////////////////////////////////////////////

class Pipe
{
public:
	inline static unsigned idIncr = 0;
	unsigned id;
	uint16_t headX = 0;
	uint16_t headY = 0;
	uint8_t direction; // 0 north, 1 east, 2 south, 3 west
	uint8_t rgb[3] = {0};
	int *termWidth;
	int *termHeight;
	static constexpr uint8_t rotProba = 20; // out of 128
	// Value is adjusted later

	uint16_t pipe_pos[116*2] = {0}; // Stores XY for each pipe already drawn.
	// 116 is the number of shades of the color (starting from 255) that i
	// will get if i fo x * 250 / 255 after each frame
	// so at frame 117 the first pixel will be black
	uint_fast8_t index = 0; // Index of the last pixel drawn

	Pipe(int *w, int *h) :
		id(idIncr++),
		headX(rand() % *w), headY(rand() % *h),
		direction(rand() & 0b11),
		termWidth(w), termHeight(h)
	{
		const uint8_t *val = HUE_LUT.data() + (rand() & 0xFF)* 3;
		rgb[0] = *val, rgb[1] = *++val, rgb[2] = *++val;
	}

	void move()
	{
		// The logic is around 4 tasks:
		// - select the correct symbol to print
		// - change the direction randomly
		// - go in this direction
		// - append the char pos to the list so it can be modified later
		char8_t symb = u8'+';
		symb = SYMBOLS[direction&1];

		// Change direction
		// If the movement is horizontal, the chances to change dir are divided
		// by 2 because the format of a monospace char is 2:1
		uint8_t randN = rand() & 0xFF;
		// If direction is odd the mov is horizontal
		// so we multiply rot proba by 2
		uint8_t compProb = (rotProba << ((~direction)&1)) - 1;
		if (randN <= compProb) {
			++direction &= 0b11;
		} else if (randN >= 0xFF - compProb) {
			(direction += 3) &= 0b11;
		}

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
		default:
			fprintf(stderr, "Direction error");
			break;
		}

		print_char_at_rgb(headX, headY, rgb, symb);
	}
};

class Pipes
{
private:
	std::vector<Pipe> pipes;
public:
	int terminalHeight = 0;
	int terminalWidth = 0;

	Pipes()
	{
		get_terminal_size(terminalWidth, terminalHeight);
		pipes.emplace_back(Pipe(&terminalWidth, &terminalHeight));
		pipes.emplace_back(Pipe(&terminalWidth, &terminalHeight));
		pipes.emplace_back(Pipe(&terminalWidth, &terminalHeight));
		pipes.emplace_back(Pipe(&terminalWidth, &terminalHeight));
	}
	// Next frame
	void next_frame()
	{
		for (auto &pipe : pipes)
			pipe.move();
	}
};

////////////////////////////////////////////////////////////////////////////////

int main()
{
	srand(time(NULL));

	Pipes pipes;
	using clock = std::chrono::steady_clock;
    constexpr auto frame_time = std::chrono::milliseconds(50); // 20 FPS

	auto next = clock::now();

	// Set bold
	write(1, "\e[1m", 4);

	while (true) {
		next += frame_time;
		pipes.next_frame();
		fflush(stdout);

		std::this_thread::sleep_until(next);
	}
	return 0;
}
