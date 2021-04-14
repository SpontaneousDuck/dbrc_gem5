```
docker build -t dbrc_gem5:latest .
```
```
docker run --rm -ti -v $PWD:/root/workspace -u $(id -u ${USER}):$(id -g ${USER}) dbrc_gem5 gem5.opt --outdir=output run_dbrc_cache.py
```