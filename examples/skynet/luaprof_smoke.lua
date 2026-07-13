local skynet = require "skynet"
require "skynet.manager"

skynet.start(function()
    io.stderr:write("luaprof skynet smoke: ok\n")
    skynet.abort()
end)
