/* userland/os2x86/test/ow/cpptest.cpp - C++: static constructors, iostreams, the STL, exceptions */
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <stdexcept>
#include <cstdio>

struct Global { int v; Global() : v(42) {} ~Global() { std::printf("global destructor ran\n"); } };
static Global g;

struct Shape { virtual ~Shape() {} virtual double area() const = 0; };
struct Sq : Shape { double s; Sq(double x) : s(x) {} double area() const { return s * s; } };
struct Ci : Shape { double r; Ci(double x) : r(x) {} double area() const { return 3.14159 * r * r; } };

static int depth(int n) { if (n == 0) throw std::runtime_error("bottom"); return depth(n - 1) + 1; }

int main()
{
    int fails = 0;
#define CHECK(c, w) do { bool _c = (c); std::cout << (_c ? "ok " : "FAIL ") << w << std::endl; if (!_c) fails++; } while (0)
    CHECK(g.v == 42, "static constructor");
    std::string s = "Hello";
    s += ", OS/2";
    CHECK(s == "Hello, OS/2" && s.length() == 11, "std::string");
    std::vector<int> v;
    for (int i = 10; i > 0; i--) v.push_back(i * 3);
    std::sort(v.begin(), v.end());
    CHECK(v.front() == 3 && v.back() == 30 && v.size() == 10, "vector, sort");
    std::map<std::string, int> m;
    m["one"] = 1; m["two"] = 2; m["three"] = 3;
    CHECK(m["two"] == 2 && m.size() == 3, "map");
    Shape *sh[2] = { new Sq(2), new Ci(1) };
    double tot = 0;
    for (int i = 0; i < 2; i++) { tot += sh[i]->area(); delete sh[i]; }
    CHECK(tot > 7.14 && tot < 7.15, "virtual functions, new/delete");
    bool caught = false;
    try { depth(5); } catch (const std::exception &e) { caught = std::string(e.what()) == "bottom"; }
    CHECK(caught, "exception through 5 frames");
    try { throw 17; } catch (int x) { caught = x == 17; }
    CHECK(caught, "throw int");
    std::cout << "pi is about " << 3.14159 << ", " << 255 << " in hex is " << std::hex << 255 << std::dec << std::endl;
    std::cout << (fails ? "some tests failed" : "all passed") << std::endl;
    return fails;
}
