-- The command of the two rules in build.ninja (docs/slices/realninja.md): ninja on Windows runs a rule's command
-- line with CreateProcess, no cmd.exe, so the rules run the guest's lua.exe with this script.
--   lua.exe ninja_tool.lua copy   <in> <out>
--   lua.exe ninja_tool.lua concat <out> <in>...
-- Files are read and written in binary mode.
local function read(path)
  local f, err = io.open(path, "rb")
  if not f then io.stderr:write("ninja_tool: ", tostring(err), "\n") os.exit(2) end
  local data = f:read("a")
  f:close()
  return data
end

local function write(path, data)
  local f, err = io.open(path, "wb")
  if not f then io.stderr:write("ninja_tool: ", tostring(err), "\n") os.exit(3) end
  f:write(data)
  f:close()
end

local verb = arg[1]
if verb == "copy" and #arg == 3 then
  write(arg[3], read(arg[2]))
elseif verb == "concat" and #arg >= 3 then
  local parts = {}
  for i = 3, #arg do parts[#parts + 1] = read(arg[i]) end
  write(arg[2], table.concat(parts))
else
  io.stderr:write("usage: ninja_tool.lua copy <in> <out> | concat <out> <in>...\n")
  os.exit(1)
end
