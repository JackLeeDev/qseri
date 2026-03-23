# qseri  
**A lua serialize library from quick engine**  
 
✨ **Core Features**  
- 🪶 **Ultra-compact encoding(49% smaller)** 
- 🚀 **Blazing-fast decoding(40% faster)**

# Data:
```lua 
local msg = {
  items = {
      {itemId = 10001, itemNum = 1, name = "item1", props = {atk = 1, def = 1}},
      {itemId = 10002, itemNum = 1, name = "item2", props = {atk = 2, def = 2}},
      {itemId = 10003, itemNum = 1, name = "item3", props = {atk = 1, def = 1}},
      {itemId = 10004, itemNum = 1, name = "item4", props = {atk = 2, def = 2}},
      {itemId = 10005, itemNum = 1, name = "item5", props = {atk = 1, def = 1}},
      {itemId = 10006, itemNum = 1, name = "item6", props = {atk = 2, def = 2}},
      {itemId = 10007, itemNum = 1, name = "item7", props = {atk = 1, def = 1}},
      {itemId = 10008, itemNum = 1, name = "item8", props = {atk = 2, def = 2}},
      {itemId = 10009, itemNum = 1, name = "item9", props = {atk = 1, def = 1}},
      {itemId = 100010, itemNum = 1, name = "item10", props = {atk = 2, def = 2}},
  },
}

local buffer,sz = qseri.raw_pack(msg)
local deMsg = qseri.unpack(buffer, sz)

local buffer2,sz2 = qseri.compress_pack(msg)
local deMsg = qseri.unpack(buffer2, sz2)
```

# Test 100 million times(Intel(R) Core(TM) i5-13400F):

| Protocol   | Encoded Size | Encode Time | Decode Time | Total Time |
|------------|-------------:|------------:|------------:|-----------:|
| **lua-serialize** | 543 bytes     | 3777 ms      | 6070 ms     | 10030 ms    |
| **qseri(raw_pack)** | 491 bytes     | 3733 ms      | **3551 ms**      | **7284 ms** |
| **qseri(compress_pack)** | **275 bytes**     | 5037 ms      | **3437 ms**      | 8474 ms |

lua-serialize : https://github.com/cloudwu/lua-serialize