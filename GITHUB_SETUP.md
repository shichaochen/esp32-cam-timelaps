# GitHub 推送指南

## 步骤 1: 在 GitHub 上创建新仓库

1. 访问 https://github.com/new
2. 填写仓库名称（例如：`esp32-cam-timelaps`）
3. 选择 Public 或 Private
4. **不要**勾选 "Initialize this repository with a README"（因为我们已经有了）
5. 点击 "Create repository"

## 步骤 2: 连接本地仓库到 GitHub

复制 GitHub 提供的仓库 URL（例如：`https://github.com/yourusername/esp32-cam-timelaps.git`）

然后在终端运行：

```bash
cd /Users/shichaochen/cursor/esp32-cam-timelaps

# 添加远程仓库（替换为您的实际仓库URL）
git remote add origin https://github.com/yourusername/esp32-cam-timelaps.git

# 推送到 GitHub
git branch -M main
git push -u origin main
```

## 如果使用 SSH（推荐）

如果您配置了 SSH 密钥，可以使用 SSH URL：

```bash
git remote add origin git@github.com:yourusername/esp32-cam-timelaps.git
git push -u origin main
```

## 后续推送

之后只需要：

```bash
git add .
git commit -m "您的提交信息"
git push
```


