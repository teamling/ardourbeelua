local thread = require "bee.thread"

local cpath_template = ("package.cpath = [[%s]]\n"):format(package.cpath)

thread.thread(cpath_template .. [[
    local thread = require "bee.thread"
    local err = thread.channel "errlog"
    while true do
        print(errlog:bpop())
    end
]])

local function eq2(a, b)
    local delta = a - b
    return delta < 0.01 and delta > -0.01
end

local function file_exists(name)
    local f = io.open(name, 'r')
    if f then
        f:close()
        return true
    else
        return false
    end
end

-- thread.sleep
local clock = os.clock()
thread.sleep(0.1)
assert(eq2(os.clock() - clock, 0.1))

-- thread.thread
os.remove('temp')
thread.sleep(0.1)
assert(file_exists('temp') == false)
thread.thread(cpath_template .. [[
    io.open('temp', 'w'):close()
]])
thread.sleep(0.1)
assert(file_exists('temp') == true)
os.remove('temp')

-- wait
os.remove('temp')
local thd = thread.thread(cpath_template .. [[
    local thread = require 'bee.thread'
    thread.sleep(0.1)
    io.open('temp', 'w'):close()
]])
assert(file_exists('temp') == false)
local clock = os.clock()
thd:wait()
assert(eq2(os.clock() - clock, 0.1))
assert(file_exists('temp') == true)
os.remove('temp')

-- thread.newchannel
-- thread.channel
local suc, c = pcall(thread.channel, 'test')
assert(suc == false)

thread.newchannel 'test'
local suc, c = pcall(thread.channel, 'test')
assert(suc == true)
assert(c ~= nil)

local suc = pcall(thread.newchannel, 'test')
assert(suc == false)

-- thread.reset
thread.newchannel 'reset'
local suc, c = pcall(thread.channel, 'reset')
assert(suc == true)
assert(c ~= nil)

thread.reset()

local suc, c = pcall(thread.channel, 'reset')
assert(suc == false)

-- push
-- bpop
thread.newchannel 'request'
thread.newchannel 'response'
thread.thread(cpath_template .. [[
    local thread = require "bee.thread"
    local request = thread.channel 'request'
    local response = thread.channel 'response'
    local msg = request:bpop()
    if msg == 'request' then
        response:push('response')
    end
]])

local request = thread.channel 'request'
local response = thread.channel 'response'
request:push('request')
local msg = response:bpop()
assert(msg == 'response')

thread.thread(cpath_template .. [[
    local thread = require "bee.thread"
    local request = thread.channel 'request'
    local response = thread.channel 'response'
    local msg = request:bpop()
    if msg == 'request' then
        thread.sleep(0.1)
        response:push('response')
    end
]])

local clock = os.clock()
request:push('request')
local msg = response:bpop()
assert(msg == 'response')
assert(eq2(os.clock() - clock, 0.1))

thread.thread(cpath_template .. [[
    local thread = require "bee.thread"
    local request = thread.channel 'request'
    local response = thread.channel 'response'
    while true do
        local op, data = request:bpop()
        if op == 'echo' then
            response:push(data)
        elseif op == 'plus' then
            response:push(data[1] + data[2])
        end
    end
]])

request:push('echo', nil)
local result = response:bpop()
assert(result == nil)

request:push('echo', 5)
local result = response:bpop()
assert(result == 5)

request:push('echo', 'test')
local result = response:bpop()
assert(result == 'test')

request:push('plus', {1, 2})
local result = response:bpop()
assert(result == 3)

thread.reset()