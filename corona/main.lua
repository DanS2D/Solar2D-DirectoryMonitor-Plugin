-----------------------------------------------------------------------------------------
--
-- main.lua
--
-----------------------------------------------------------------------------------------

local directoryMonitor = require("plugin.directoryMonitor")
local docsPath = system.pathForFile("", system.DocumentsDirectory)
local count = 0

local function directoryListener(event)
    count = count + 1

    for k, v in pairs(event) do
        print(k, v)
    end

    print(string.format("-------COUNT = %s---------", count))

end

Runtime:addEventListener("directoryMonitor", directoryListener)

local watchID = directoryMonitor.watch("C:\\")
