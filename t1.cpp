#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>

int main()
{
	std::string command = "/usr/bin/python3 -c 'print(123)'";
	FILE *pipe = popen(command.c_str(), "w+");
	if (!pipe)
	{
		std::cerr << "popen failed (" << errno << "): " << strerror(errno) << "\n";
		return 1;
	}
	std::array<char, 128> buf{};
	while (fgets(buf.data(), buf.size(), pipe))
		std::cout << buf.data();
	pclose(pipe);
}
