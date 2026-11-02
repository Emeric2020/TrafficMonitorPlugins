# 本机构建与部署

- 每次修改源码或影响运行结果的配置后，都要构建受影响的目标，并部署到本机正在使用的 TrafficMonitor，再确认部署产物与构建产物一致。
- TrafficMonitor 宿主源码位于相邻的 `TrafficMonitor` 仓库；只改宿主时构建宿主，只改插件时构建对应插件，两者都改时两者都构建。
- 当前本机运行目录为 `D:\Projects\TrafficMonitor`。修改 CodexUsage 后构建本仓库 `Plugins\CodexUsage\CodexUsage.vcxproj` 的 x64 Release，再运行宿主仓库的 `RestartTrafficMonitor.ps1` 部署并重启。
- 部署时保留用户现有配置和偏好；除非用户明确要求，不导入预设设置或更改插件启用状态。
