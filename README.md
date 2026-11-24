# Simulador de memória virtual

O programa traduz endereços lógicos de 16 bits usando páginas de 256 bytes, 128 frames de memória física e uma TLB com 16 entradas. Há substituição FIFO e LRU tanto para páginas quanto para a TLB. Cada consulta à TLB cria uma thread para cada entrada e reúne os resultados antes de continuar a tradução.

## Compilação e execução

Deixe `BACKING_STORE.bin` no diretório do programa e compile com:

```sh
make
```

A execução recebe o arquivo de endereços, o algoritmo das páginas e o algoritmo da TLB, nessa ordem:

```sh
./vm addresses.txt lru fifo
```

Os algoritmos aceitos são `fifo` e `lru`. O resultado é gravado em `correct.txt`, com uma linha por endereço e cinco linhas finais para as estatísticas. `make clean` remove o executável e o arquivo de saída.

O programa encerra com erro quando a quantidade de argumentos está incorreta, uma política é desconhecida, o arquivo de endereços contém uma linha inválida ou o backing store não possui exatamente 65.536 bytes. A saída só é criada depois da validação dos arquivos. Se ocorrer uma falha durante a escrita, o arquivo parcial é removido.
