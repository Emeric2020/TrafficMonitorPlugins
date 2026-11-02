# Qoder Usage Plugin

TrafficMonitor 的 Qoder / WorkBuddy 额度监控插件。自动读取当前 Windows 用户已登录的桌面应用凭据，无需配置 token。

## 显示项目与命名（0.4.0）

显示设置、任务栏文字、详情标题、应用按钮及插件命令统一使用以下短名称；无 `CN` 后缀为国际版。

| 名称 | 版本 | 显示项 ID | 独立历史文件 |
| --- | --- | --- | --- |
| Qoder | 国际版 | `QoderUsageQuota9` | `QoderUsage.history` |
| QoderCN | 中国版 | `QoderCNUsageQuota` | `QoderCNUsage.history` |
| WorkBuddy | 国际版 | `WorkBuddyInternationalCredits` | `WorkBuddyInternationalUsage.history` |
| WorkBuddyCN | 中国版 | `WorkBuddyCredits` | `WorkBuddyUsage.history` |

原有三个 ID 和历史文件名保持兼容；新增 QoderCN 不会改变已有显示勾选。右键 TrafficMonitor 任务栏 → **显示设置** → 勾选需要显示的项目。四项分别查询、分别发布状态，失败不会覆盖其他项；旧 `Region` 选项保留配置兼容，界面显示为按项目固定区域，不能再把国际版项目切换到中国版。


## WorkBuddy / WorkBuddyCN 积分

`WorkBuddy` 读取国际版 `WorkBuddyAI.exe` 的登录状态，`WorkBuddyCN` 读取中国版 `WorkBuddy.exe` 的登录状态，原中国版显示项仅更名，ID 不变。插件命令分别提供两项详情。

- 第一行：十段剩余积分条、剩余百分比和北京时间自然日倒计时。
- 第二行明确显示版本，例如 `WorkBuddyCN 2200/2200`、`WorkBuddy 350/350`，背景绘制各自最近 24 小时消耗。刷新频率、主题和工作时间安排与 Qoder 共用。
- 左键打开对应版本详情，标题、区域（中国 cn / 国际 global）、登录来源均区分版本；“打开 WorkBuddyCN”或“打开 WorkBuddy”按钮启动对应客户端。
- 中国版使用 `WorkBuddyUsage.history`，国际版使用 `WorkBuddyInternationalUsage.history`，与 `QoderUsage.history` 分开；版本、账号或总量变化时重建相应基线。一版失败不会覆盖另一版余额。请求失败时显示占位符及错误状态，不把旧值冒充为最新余额。

### 已核实的接口与开发要点

2026-11-02 对照本机客户端及开源实现，验证两版均使用以下只读链路（中国版示例，国际版域名为 `www.workbuddy.ai`）：

```text
POST https://www.workbuddy.cn/billing/meter/get-user-resource-summary
Authorization: Bearer <桌面 accessToken>
X-User-Id: <当前账号 uid>
Content-Type: application/json

{}
```

成功要求 HTTP 2xx 且 JSON `code: 0`。从 `data.Packages[]` 汇总 `CapacityUnit` 为 `"credits"` 或 `"credit"` 的 `CycleTotalCapacity` 和 `CycleRemainCapacity`；数字和十进制字符串都支持。国际版新用户赠送包实测使用单数 `credit`，不能只识别复数，否则会漏算 250 积分。`CreditGroups` 是同一余额的另一种分组，不能再次累计。`CycleFrozenCapacity` 单独展示，不重复扣减剩余量。余额超过当期发放量时保留实际余额，进度条最多填满；空包列表显示已确认的 `0/0`，结构异常不会转换为零余额。

登录文件均位于 `%LOCALAPPDATA%\CodeBuddyExtension\Data\Public\auth`，按版本严格匹配，不跨版回退：

| 版本 | 登录文件 | 默认安装目录下的程序 | 允许的请求域名 |
| --- | --- | --- | --- |
| 中国版 | `workbuddy-desktop.info` | `WorkBuddy\WorkBuddy.exe` | `www.workbuddy.cn` |
| 国际版 | `workbuddy-desktop-ai.info` | `WorkBuddyAI\WorkBuddyAI.exe` | `www.workbuddy.ai` |

尊重各自的 `.logged-out` 标记。新版 `$wbEncrypted` 字段使用 AES-256-GCM。插件以隐藏的 Node 模式短暂启动对应版本的客户端，通过它自身的 `electron_browser_workbuddy_storage` 读取构建密钥并在该子进程完成解密及查询。向宿主回传的仅为选定积分字段和不可逆账号摘要；令牌、密钥不写盘、不记日志、不进入命令行，也不刷新或修改登录态。无额外 Node/Python 运行依赖；单次请求限时 15 秒，子进程总限时 25 秒，拒绝重定向和与当前版本不匹配的登录域名。

参考资料：

- [WorkBuddy 官方积分说明](https://www.codebuddy.cn/docs/workbuddy/Credits)：积分扣减与有效期规则。
- [WorkBuddy Open API](https://open.workbuddy.cn/docs/openapi)：公开的积分能力是兑换；本插件采用客户端 billing 接口。
- [CodexBar WorkBuddy 接入说明](https://github.com/steipete/CodexBar/blob/main/docs/workbuddy.md)：summary 字段、分组重复计数、冻结量、Cookie 与 User-Agent 绑定、可选套餐接口。
- [zcode-workbuddy-connect](https://github.com/hawchou1995/zcode-workbuddy-connect#workbuddy-56-encrypted-credentials)：利用已安装应用运行时读取加密凭据，密钥仅留内存。

当前支持默认安装位置的 **中国版和国际版个人账号**（`Program Files` 或 `%LOCALAPPDATA%\Programs` 下的对应版本目录）。企业配额和自定义安装位置尚未接入。summary 不提供积分包到期/重置时间，插件不推测；细时间条仅代表自然日进度。billing 和桌面加密方式属于客户端内部接口，升级后若变化需适配。

### 验证记录

0.4.0（2026-11-02）：Release x64 构建通过。新增 `QoderDesktopAuthTests.cpp`，使用合成 DPAPI/AES-GCM 登录文件验证两版分别读取、QoderCN 缺失时不得回退国际版，以及真实本机两版凭据读取；原 WorkBuddy 解析测试继续通过。动态加载构建 DLL 验证四个准确短名称、原 ID 保留、真实轮询和浅/深色 16/24 像素字号绘制，实测 `Qoder 2180/2700`、`QoderCN 100/100`、`WorkBuddy 350/350`、`WorkBuddyCN 2200/2200`，关闭正常。已部署到本机并正常重启，单个宿主进程和插件加载路径确认，构建/安装 SHA256 一致，保留已有配置与显示选择；宿主已生成独立 `QoderCNUsage.history`。

0.3.0（2026-11-02）：Release x64 构建通过，真实接口确认中国版 `2200/2200`、国际版 `350/350`。新增国际版单数 `credit` 赠送包、两版同 UID 的历史隔离及官网地址隔离回归检查。动态加载 DLL，验证三个显示项、原中国版 ID 保持不变、两版中文名称、后台分别查询、浅/深色及 16/24 像素字号绘制和正常关闭。已部署并正常重启本机 TrafficMonitor，确认单个宿主进程、加载插件路径、构建/安装 SHA256 一致，两个版本均生成各自历史文件；安装后 DLL 的真实查询和绘制再次通过。保留宿主退出时保存的已有显示选择。应用按钮实际点击与长期运行仍待人工验收。

0.2.0（2026-11-02）：Release x64 构建通过；`WorkBuddyUsageTests.cpp` 覆盖数字字符串、冻结量、重复分组、单位过滤、异常结构、业务失败、零余额及超额余额，真实 API 查询返回 `2200/2200`。动态加载构建 DLL，确认当时两个显示项、原 Qoder ID、后台真实轮询、浅/深色与 16/24 像素字号绘制及正常关闭。已部署到本机 `D:\Projects\TrafficMonitor\plugins\QoderUsage.dll`，正常重启后确认单个宿主进程、加载路径和构建/部署 SHA256 一致，插件启用与显示选择保持原状，实际宿主生成了独立 WorkBuddy 历史文件；安装后的 DLL 再次真实查询与绘制检查通过。应用按钮点击与长期运行仍待人工验收。

在 VS x64 开发者命令行中，从仓库根目录可单独运行解析和真实接口检查：

```bat
cl /nologo /EHsc /std:c++17 /utf-8 /wd4100 /I include Plugins\QoderUsage\WorkBuddyUsageTests.cpp Plugins\QoderUsage\WorkBuddyUsage.cpp /Fe:WorkBuddyUsageTests.exe /link user32.lib
WorkBuddyUsageTests.exe
WorkBuddyUsageTests.exe --live
cl /nologo /EHsc /std:c++17 /utf-8 /wd4100 Plugins\QoderUsage\QoderDesktopAuthTests.cpp Plugins\QoderUsage\QoderDesktopAuth.cpp /Fe:QoderDesktopAuthTests.exe /link crypt32.lib bcrypt.lib
QoderDesktopAuthTests.exe
QoderDesktopAuthTests.exe --live
```

## Qoder / QoderCN 任务栏显示

固定两行，字体与尺寸跟随 TrafficMonitor：

- 第一行：十段额度方块 + 剩余百分比，例如 `32%`。每段代表 10%，不足一段时从底部填充。方块宽度、间距和填充方向与 Codex 14 格压缩模式下的 5h 行一致；剩余 >50% 为绿色，>20% 且 ≤50% 为橙色，≤20% 为红色。
- 第二行：`名称 剩余额度/合计额度`，例如 `Qoder 2180/2700` 或 `QoderCN 100/100`。文字背景的柱状图和上方灰色格子反映最近 24 小时的消耗。历史通过本地轮询差值累积，首次运行后逐渐建立，保存在插件配置目录各自的历史文件；额度总量或接口区域变化时重新建立基线。

合计计算包含接口返回且可用的用户额度、附加额度、组织资源包和专属资源包，以 `合计 remaining / 合计 total` 计算百分比，不使用接口可能存在舍入差异的百分比。无数据时显示占位符。

方块下方的细时间条是**当前北京时间自然日 00:00 到次日 00:00 的倒计时**，按距离次日 00:00 的剩余时间从右侧缩短；工作/休息配色和工作时段边界与 Codex 5h 条一致。选项中可设置单双休安排，以及上午、下午工作时段的起止时间。它不表示套餐到期或已领取状态，也不会自动领取活动奖励。第二行倒计时仍表示接口返回的额度周期到期时间。

时间条复用 Codex 5h 条的工作/休息配色、配置的工作时段边界和工作日安排。读取 DLL 旁的 `calendar/YYYY.txt`，支持节假日和调休；文件格式见 [Codex 日历说明](../CodexUsage/README.md#configurable-workdays-and-holidays)。日历随用量刷新重新加载，无文件时按周一至周五判断。

## 详情气泡与应用按钮

左键点击打开详情，再次点击、Escape、关闭按钮或鼠标离开气泡及原点击位置后隐藏。打开后的 600ms 提供移入宽限期，鼠标停留在原点击位置时保持显示。

详情包含连接状态、区域、凭据来源、刷新时间、套餐、接口百分比、额度周期和各类额度。右上角应用按钮分别启动 `%LOCALAPPDATA%\Programs\Qoder\Qoder.exe` 或 `%LOCALAPPDATA%\Programs\Qoder CN\Qoder CN.exe`，QoderCN 不会打开国际版客户端。应用需安装在此用户安装位置。

Qoder 或 Codex 详情可见时，对应宿主窗口的原生提示气泡暂停显示，包括宿主再次请求显示提示的情况；详情关闭后恢复。多个插件同时打开详情时，全部关闭才恢复共享的宿主提示。

## 构建与安装

使用 Visual Studio 2022 的 v143 C++ 工具链和 Windows SDK。项目支持 Win32/x64，Release 和 Release (lite)；当前验证版本为 Release x64。

```powershell
# 在 Visual Studio 开发者 PowerShell 中运行
MSBuild.exe Plugins\QoderUsage\QoderUsage.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir="$PWD\" /m
```

产物为 `bin/x64/Release/QoderUsage.dll`。退出 TrafficMonitor 后复制到实际运行目录的 `plugins/`，与 Codex 共用 `plugins/calendar/`。当前本机部署位置是 `D:\Projects\TrafficMonitor\plugins`。重启后在任务栏显示设置中选择所需短名称项目。升级保留已有日历和配置。

## 配置与凭据

选项提供刷新频率（1/5/15/60 分）、主题、固定区域说明、低额度预警阈值，保存在宿主插件配置目录的 `QoderUsage.ini`。旧选项中的进度条样式、合计/周期开关仍保留配置兼容，但不改变当前固定十段、两行任务栏布局。

仅支持桌面应用 OAuth token；Personal Access Token 不适用于此用量接口。凭据分别探测当前用户 `%APPDATA%` 下对应区域的目录，每个目录都需要 `Local State` 和 `auth.v1.dat`：

| 名称 | 首选目录 | 兼容目录 | 唯一 API 域名 |
| --- | --- | --- | --- |
| Qoder | `com.qoder.app.stable` | `com.qoder.app` | `openapi.qoder.sh` |
| QoderCN | `com.qodercn.app.stable` | `com.qodercn.app` | `openapi.qoder.com.cn` |

使用当前用户 DPAPI + AES-256-GCM 解密。只读取凭据，不替桌面应用刷新 token。过期时打开对应客户端登录，后续轮询重新读取凭据。QoderCN 缺少凭据时明确报错，不回退到 Qoder；国际版同样不读取中国版凭据。中国区官方 OpenAPI 域名可参考 [Qoder CN 文档](https://docs.qoder.cn/en/cloud-agents/quickstart)。

两项使用相同用量路径 `GET /sash/api/v2/me/usage`，各自固定上述区域，不再按全局 Region 设置跨区重试。WinHTTP 连接前解析 HTTPS URL，只向连接函数传入主机名及端口。

## 限制与验证

- Windows 10+，需要支持 API 9 关闭回调的 TrafficMonitor 宿主。
- 企业账户无额度明细时不能绘制合计额度。
- 凭据仅用于 HTTPS Authorization，不写入日志或配置。
- 活动额度与试用额度各有到期规则；10:00 时间条不是自动补发套餐额度的承诺。
- 已通过 Release x64 构建、真实 API 读取、额度比例与时间边界检查、提示窗口再次显示拦截/恢复检查；本机已确认进程路径、加载 DLL 和 SHA256。应用按钮点击及最终 UI 外观仍需人工验收。
