ConnectCoin Core - instalador Windows x64
========================================

Este pacote instala o Core grafico e as ferramentas de linha de comando.
Requer Windows 10 1809 ou posterior (64 bits), incluindo Windows 11.
As bibliotecas Qt e Visual C++ acompanham o pacote: nao e necessario instalar
Visual Studio, Python ou baixar dependencias para abrir o Core.

Abra ConnectCoin Core pelo menu Iniciar ou pelo atalho na area de trabalho.
Os executaveis ficam na pasta bin do diretorio escolhido na instalacao.
A rede padrao e mainnet; uma configuracao existente pode selecionar outra rede.
O instalador nao inicia mineracao, claims, RPC ou servicos automaticamente.

Se ja houver um Core aberto, feche-o normalmente antes de atualizar, reparar
ou desinstalar. Espere o processo encerrar e gravar seus dados. O instalador
nao encerra forcadamente a carteira ou o no.

Seus dados normalmente ficam em %LOCALAPPDATA%\ConnectCoin (ou no diretorio
que voce escolher no Core). Instalacao, reparo e desinstalacao NAO removem
carteiras, blockchain, configuracao ou backups desse diretorio.
Nao use o diretorio de instalacao como diretorio de dados.

O Core fica disponivel como opcao para abrir links connectcoin: nas
Configuracoes do Windows > Aplicativos > Aplicativos padrao. O instalador
nao substitui a escolha de outro aplicativo, como a ConnectWallet.

Ferramentas (PowerShell, dentro da pasta bin):
  .\connectcoin-cli.exe -help
  .\connectcoind.exe -help
  .\connectcoin-wallet.exe -help
  .\connectcoin-tx.exe -help
  .\connectcoin-util.exe -help
O helper share\rpcauth\rpcauth.py e opcional e requer Python 3.
share\examples\connectcoin.conf e apenas um exemplo: nao e ativado automaticamente.
Nenhuma porta ou permissao de firewall e alterada pelo MSI.

Licencas e codigo-fonte
----------------------
Core: COPYING.txt; dependencias: licenses; versoes: build-info.json.
Codigo e instrucoes de compilacao: https://github.com/connectcoincrypto/connectcoin
Qt e distribuido como DLLs substituiveis, nos termos aplicaveis da LGPL/GPL.
Codigo-fonte das versoes Qt: https://download.qt.io/official_releases/qt/
Receitas e patches de dependencias: https://github.com/microsoft/vcpkg
Microsoft Visual C++ Runtime: distribuido sob os termos Microsoft aplicaveis.
As DLLs locais Visual C++ precisam ser atualizadas em novos pacotes quando
a Microsoft publicar correcoes; nao sao mantidas por este instalador em segundo plano.
