-- The first program that is not ours runs this through win10xbox.exe: the Lua 5.4 interpreter built from the
-- upstream sources (tools/ci/build_lua.ps1), as `lua.exe C:\guest\lua_smoke.lua alpha "beta gamma"`.
-- Every section prints "smoke: <name> ok" or "smoke: <name> FAILED ..."; the exit code is 42 when all passed
-- (tests/integration/CMakeLists.txt gates on the lines and the code).
local fails = 0
local function check(name, cond, detail)
  if cond then
    print("smoke: " .. name .. " ok")
  else
    fails = fails + 1
    print("smoke: " .. name .. " FAILED " .. tostring(detail))
  end
end

-- the command line reached the program intact (each argument exactly, last one NUL-terminated)
check("argv", #arg == 2 and arg[1] == "alpha" and arg[2] == "beta gamma" and arg[0]:lower():find("lua_smoke.lua", 1, true),
      table.concat(arg, "|"))

-- string.format
check("format", string.format("%5d|%-5s|%05.1f|%x|%q", 42, "ab", 3.14159, 255, "a\nb") == '   42|ab   |003.1|ff|"a\\\nb"',
      string.format("%5d|%-5s|%05.1f|%x", 42, "ab", 3.14159, 255))
check("format.g", string.format("%g %g %.3f", 1e20, 0.1, 2 / 3) == "1e+20 0.1 0.667")

-- tables and sort
local t = {}
for i = 1, 1000 do t[i] = (i * 7919) % 1009 end
table.sort(t)
local sorted = true
for i = 2, #t do if t[i - 1] > t[i] then sorted = false end end
table.sort(t, function(a, b) return a > b end)
check("sort", sorted and t[1] >= t[#t] and #t == 1000)
local words = { "pear", "apple", "fig", "banana" }
table.sort(words)
check("sort.words", table.concat(words, ",") == "apple,banana,fig,pear")

-- io.write to stdout (returns the file, so calls chain)
io.write("smoke: io.write ", 1, " ", 2.5, " ", "x", "\n")
check("io.write", io.write("") == io.stdout)

-- os.time / os.date / os.clock
local now = os.time()
check("os.time", math.type(now) == "integer" and now > 1700000000, now)
local stamp = os.time { year = 2001, month = 2, day = 3, hour = 4, min = 5, sec = 6 }
check("os.date", os.date("%Y-%m-%d %H:%M:%S", stamp) == "2001-02-03 04:05:06", os.date("%Y-%m-%d %H:%M:%S", stamp))
local d = os.date("*t", stamp)
check("os.date.t", d.year == 2001 and d.month == 2 and d.day == 3 and d.hour == 4 and d.min == 5 and d.sec == 6)
local c0 = os.clock()
local x = 0
for i = 1, 2000000 do x = x + i % 7 end
local c1 = os.clock()
check("os.clock", c0 >= 0 and c1 >= c0 and x > 0, c0 .. " " .. c1)

-- the environment: the layer's guest user profile
local home = os.getenv("USERPROFILE")
check("getenv", home == [[C:\Users\User]], home)
check("getenv.none", os.getenv("WIN10XBOX_SURELY_UNSET") == nil)

-- a file under the guest's home: write, read back, size, rename, remove
local path = home .. [[\lua_smoke.tmp]]
local f, err = io.open(path, "wb")
check("io.open.write", f ~= nil, err)
if f then
  f:write("line one\n", "line two\n", string.rep("z", 100000), "\n")
  check("file.close", f:close())
  local r = io.open(path, "rb")
  check("io.open.read", r ~= nil)
  if r then
    check("file.line1", r:read("l") == "line one")
    check("file.line2", r:read("l") == "line two")
    local rest = r:read("a")
    check("file.rest", #rest == 100001, #rest)
    check("file.size", r:seek("end") == 9 + 9 + 100001, r:seek("end"))
    r:close()
  end
  local moved = path .. ".moved"
  check("os.rename", os.rename(path, moved))
  check("os.remove", os.remove(moved))
  local gone = io.open(moved, "rb")
  check("file.gone", gone == nil)
end
local missing, merr = io.open(home .. [[\no\such\dir\file.txt]], "rb")
check("io.open.missing", missing == nil and type(merr) == "string" and merr:find("No such file", 1, true), merr)

-- the temp directory, io.lines, dofile and require
local tmp = os.tmpname()
check("os.tmpname", type(tmp) == "string" and #tmp > 0, tmp)
local tf = io.open(tmp, "w")
check("tmpname.open", tf ~= nil, tmp)
if tf then
  tf:write("alpha\nbeta\ngamma\n")
  tf:close()
  local n, last = 0, nil
  for line in io.lines(tmp) do n = n + 1; last = line end
  check("io.lines", n == 3 and last == "gamma", n)
  os.remove(tmp)
end
local modpath = home .. [[\lua_smoke_mod.lua]]
local mf = io.open(modpath, "w")
mf:write("return { answer = 42 }\n")
mf:close()
check("dofile", dofile(modpath).answer == 42)
package.path = home .. [[\?.lua]]
check("require", require("lua_smoke_mod").answer == 42)
os.remove(modpath)

-- pcall / error
local ok, e = pcall(function() error("boom") end)
check("pcall", ok == false and e:find("boom", 1, true))
local ok2, e2 = pcall(error, { code = 7 })
check("pcall.table", ok2 == false and e2.code == 7)
local ok3, e3 = pcall(function() local z = nil; return z.field end)
check("pcall.runtime", ok3 == false and e3:find("attempt to index a nil value", 1, true), e3)
check("xpcall", select(2, xpcall(function() error("tb") end, function(m) return "handled:" .. m end)):find("handled:", 1, true))

-- string.rep on 1 MB, and what a program does with it
local big = string.rep("0123456789abcdef", 65536)
check("string.rep", #big == 1048576, #big)
check("string.rep.sub", big:sub(-16) == "0123456789abcdef" and select(2, big:gsub("abc", "")) == 65536)
check("string.rep.sep", #string.rep("ab", 1000, ",") == 2 * 1000 + 999)
local parts = {}
for i = 1, 20000 do parts[i] = i end
check("table.concat", #table.concat(parts, ",") == 108893)
collectgarbage()
check("gc", collectgarbage("count") > 0)

print(string.format("smoke: done fails=%d", fails))
os.exit(fails == 0 and 42 or 1)
