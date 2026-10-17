# 生成许可验签用的 RSA-2048 钥匙对（设计《notepadcc许可与激活设计.md》3.4）。
#
# 用法：powershell -File tools\make_license_key.ps1 lic-2026a
#
# 输出：keyId、公钥（BCrypt RSAPUBLICBLOB 的 base64，抄进 src/license/LicenseKeys.h）
# 与私钥的完整 CRT 参数（n / d / p / q / dp / dq / iq，十六进制）——私钥只给
# 离线签发环境与"造测试许可"的脚本用。
#
# 正式发布时：在**离线**机器上运行本脚本，私钥单独保管，绝不可把正式私钥提交
# 进仓库（仓库里那把是一次性开发钥匙，与更新验签同一套纪律）。
param([Parameter(Mandatory=$true)][string]$KeyId)
$rsa = [System.Security.Cryptography.RSA]::Create(2048)
$parameters = $rsa.ExportParameters($true)
function ConvertTo-Hex([byte[]]$bytes) { ($bytes | ForEach-Object { $_.ToString('x2') }) -join '' }
$blob = New-Object System.Collections.Generic.List[byte]
$blob.AddRange([BitConverter]::GetBytes([uint32]0x31415352))   # 'RSA1'
$blob.AddRange([BitConverter]::GetBytes([uint32]2048))         # bitLength
$blob.AddRange([BitConverter]::GetBytes([uint32]$parameters.Exponent.Length))
$blob.AddRange([BitConverter]::GetBytes([uint32]$parameters.Modulus.Length))
$blob.AddRange([BitConverter]::GetBytes([uint32]0))            # cbPrime1
$blob.AddRange([BitConverter]::GetBytes([uint32]0))            # cbPrime2
$blob.AddRange($parameters.Exponent)
$blob.AddRange($parameters.Modulus)
Write-Output ("keyId = " + $KeyId)
Write-Output ("public = " + [Convert]::ToBase64String($blob.ToArray()))
Write-Output ("n  = " + (ConvertTo-Hex $parameters.Modulus))
Write-Output ("d  = " + (ConvertTo-Hex $parameters.D))
Write-Output ("p  = " + (ConvertTo-Hex $parameters.P))
Write-Output ("q  = " + (ConvertTo-Hex $parameters.Q))
Write-Output ("dp = " + (ConvertTo-Hex $parameters.DP))
Write-Output ("dq = " + (ConvertTo-Hex $parameters.DQ))
Write-Output ("iq = " + (ConvertTo-Hex $parameters.InverseQ))