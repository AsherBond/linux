// SPDX-License-Identifier: GPL-2.0

#include <linux/io.h>
#include <linux/ioport.h>

void __iomem *rust_helper_ioremap(phys_addr_t offset, size_t size)
{
	return ioremap(offset, size);
}

void __iomem *rust_helper_ioremap_np(phys_addr_t offset, size_t size)
{
	return ioremap_np(offset, size);
}

void rust_helper_iounmap(void __iomem *addr)
{
	iounmap(addr);
}

u8 rust_helper_readb(const void __iomem *addr)
{
	return readb(addr);
}

u16 rust_helper_readw(const void __iomem *addr)
{
	return readw(addr);
}

u32 rust_helper_readl(const void __iomem *addr)
{
	return readl(addr);
}

#ifdef CONFIG_64BIT
u64 rust_helper_readq(const void __iomem *addr)
{
	return readq(addr);
}
#endif

void rust_helper_writeb(u8 value, void __iomem *addr)
{
	writeb(value, addr);
}

void rust_helper_writew(u16 value, void __iomem *addr)
{
	writew(value, addr);
}

void rust_helper_writel(u32 value, void __iomem *addr)
{
	writel(value, addr);
}

#ifdef CONFIG_64BIT
void rust_helper_writeq(u64 value, void __iomem *addr)
{
	writeq(value, addr);
}
#endif

u8 rust_helper_readb_relaxed(const void __iomem *addr)
{
	return readb_relaxed(addr);
}

u16 rust_helper_readw_relaxed(const void __iomem *addr)
{
	return readw_relaxed(addr);
}

u32 rust_helper_readl_relaxed(const void __iomem *addr)
{
	return readl_relaxed(addr);
}

#ifdef CONFIG_64BIT
u64 rust_helper_readq_relaxed(const void __iomem *addr)
{
	return readq_relaxed(addr);
}
#endif

void rust_helper_writeb_relaxed(u8 value, void __iomem *addr)
{
	writeb_relaxed(value, addr);
}

void rust_helper_writew_relaxed(u16 value, void __iomem *addr)
{
	writew_relaxed(value, addr);
}

void rust_helper_writel_relaxed(u32 value, void __iomem *addr)
{
	writel_relaxed(value, addr);
}

#ifdef CONFIG_64BIT
void rust_helper_writeq_relaxed(u64 value, void __iomem *addr)
{
	writeq_relaxed(value, addr);
}
#endif

resource_size_t rust_helper_resource_size(struct resource *res)
{
	return resource_size(res);
}

struct resource *rust_helper_request_mem_region(resource_size_t start,
						resource_size_t n,
						const char *name)
{
	return request_mem_region(start, n, name);
}

void rust_helper_release_mem_region(resource_size_t start, resource_size_t n)
{
	release_mem_region(start, n);
}

struct resource *rust_helper_request_region(resource_size_t start,
					    resource_size_t n, const char *name)
{
	return request_region(start, n, name);
}

struct resource *rust_helper_request_muxed_region(resource_size_t start,
						  resource_size_t n,
						  const char *name)
{
	return request_muxed_region(start, n, name);
}

void rust_helper_release_region(resource_size_t start, resource_size_t n)
{
	release_region(start, n);
}
